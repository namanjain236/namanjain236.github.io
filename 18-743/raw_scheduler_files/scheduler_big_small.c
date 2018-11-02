#include "scheduler_big_small.h"
#include "simulator.h"
#include "config.hpp"
#include "thread.h"
#include "performance_model.h"
#include "core_manager.h"
#include "misc/tags.h"
#include "rng.h"
#include <math.h>

#include "subsecond_time.h"
#include "fixed_point.h"
#include "stats.h"
#include "dvfs_manager.h"

#define B_TH 2500
#define S_TH 1000

#define EMPTY_B 10000
#define EMPTY_S 0

// IPC based scheduler

SchedulerBigSmall::SchedulerBigSmall(ThreadManager *thread_manager)
   : SchedulerPinnedBase(thread_manager, SubsecondTime::NS(Sim()->getCfg()->getInt("scheduler/big_small/quantum")))
   , m_debug_output(Sim()->getCfg()->getBool("scheduler/big_small/debug"))
   , m_last_reshuffle(SubsecondTime::Zero())
   , m_rng(rng_seed(42))
{
   // Figure out big and small cores, and create affinity masks for the set of big cores and the set of small cores, respectively

   m_num_big_cores = 0;
   CPU_ZERO(&m_mask_big);
   CPU_ZERO(&m_mask_small);
   core_info info_init = {NULL, 0, NULL, 0, NULL, 0};

   for (core_id_t coreId = 0; coreId < (core_id_t) Sim()->getConfig()->getApplicationCores(); coreId++)
   {
      bool isBig = Sim()->getTagsManager()->hasTag("core", coreId, "big");
      if (isBig)
      {
         ++m_num_big_cores;
         CPU_SET(coreId, &m_mask_big);
      }
      else
      {
         CPU_SET(coreId, &m_mask_small);
      }

      core_info_list.push_back(info_init);
   }
   m_next_core = 0;
}

void SchedulerBigSmall::threadSetInitialAffinity(thread_id_t thread_id)
{
   core_id_t core_id = getFreeCore(m_next_core);
   m_next_core = getNextCore(core_id);

   m_thread_info[thread_id].setAffinitySingle(core_id);
}

void SchedulerBigSmall::threadStall(thread_id_t thread_id, ThreadManager::stall_type_t reason, SubsecondTime time)
{
   // When a thread on the big core stalls, promote another thread to the big core(s)

   if (m_debug_output)
      std::cout << "[SchedulerBigSmall] thread " << thread_id << " stalled" << std::endl;

   // Call threadStall() in parent class
   SchedulerPinnedBase::threadStall(thread_id, reason, time);

   if (m_debug_output)
      printState();
}

void SchedulerBigSmall::threadExit(thread_id_t thread_id, SubsecondTime time)
{
   // When a thread on the big core ends, promote another thread to the big core(s)

   if (m_debug_output)
      std::cout << "[SchedulerBigSmall] thread " << thread_id << " ended" << std::endl;

   for (core_id_t coreId = 0; coreId < (core_id_t) Sim()->getConfig()->getApplicationCores(); coreId++) {
   	if (m_core_thread_running[coreId] == thread_id) {
   		core_info_list[coreId].ipc = 0;
   		break;
   	}
   }

   if (m_thread_isbig[thread_id])
   {
      // Pick a new thread to run on the big core(s)
      pickBigThread();
   }

   // Call threadExit() in parent class
   SchedulerPinnedBase::threadExit(thread_id, time);

   if (m_debug_output)
      printState();
}

void SchedulerBigSmall::periodic(SubsecondTime time)
{
   bool print_state = false;
   int big_min_ipc, small_max_ipc;
   int big_core = 0; int small_core = 0;
   static int i[4] = {0, 0, 0, 0};
   static long long sum[4] = {0, 0, 0, 0};
   static float mean[4] = {0, 0, 0, 0};
   static long long sum_var[4] = {0, 0, 0, 0};

   if (time > m_last_reshuffle + m_quantum)
   {
      for (core_id_t coreId = 0; coreId < (core_id_t) Sim()->getConfig()->getApplicationCores(); coreId++) {
        if (m_core_thread_running[coreId] == INVALID_THREAD_ID) {
            core_info_list[coreId].ipc = 0;
         } else {
           get_core_ipc(time, coreId);
           i[coreId]++;
           sum[coreId] += core_info_list[coreId].ipc;
           mean[coreId] = (float)sum[coreId]/i[coreId];
           sum_var[coreId] += (long long) pow(core_info_list[coreId].ipc - (int)mean[coreId], 2);
           printf("Core %d: Mean IPC of %f, Variance is %f\n", coreId, mean[coreId], (float) sum_var[coreId]/(1000*i[coreId]));
         }
      }

      big_min_ipc = EMPTY_B;
      small_max_ipc = EMPTY_S;


      for (core_id_t coreId = 0; coreId < (core_id_t) Sim()->getConfig()->getApplicationCores(); coreId++) {
        bool isBig = Sim()->getTagsManager()->hasTag("core", coreId, "big");
        if ((isBig) && (core_info_list[coreId].ipc < big_min_ipc) && (m_core_thread_running[coreId] != INVALID_THREAD_ID)) {
        	big_min_ipc = core_info_list[coreId].ipc;
        	big_core = coreId;
        }
        if ((!isBig) && (core_info_list[coreId].ipc > small_max_ipc) && (m_core_thread_running[coreId] != INVALID_THREAD_ID)) {
        	small_max_ipc = core_info_list[coreId].ipc;
        	small_core = coreId;
        }
      }

      int thread_core_big = m_core_thread_running[big_core];
      int thread_core_small = m_core_thread_running[small_core];

      printf("Core 0: %d, Core 1: %d, Core 2: %d, Core 3: %d\t", core_info_list[0].ipc,
																 core_info_list[1].ipc,
																 core_info_list[2].ipc,
																 core_info_list[3].ipc);
      printf("%d_%d_%d_%d\n", m_core_thread_running[0], m_core_thread_running[1],
      							m_core_thread_running[2],
      							m_core_thread_running[3]);

      if (big_min_ipc == EMPTY_B) {
      	big_min_ipc = 0;
      	thread_core_big = INVALID_THREAD_ID;
      }
      if (small_max_ipc == EMPTY_S) {
      	small_max_ipc = 10000;
      	thread_core_small = INVALID_THREAD_ID;
      }

      // if ((big_min_ipc < B_TH) && (small_max_ipc > S_TH)) { //|| (core_info_list[1].ipc > 1500)) { 2000
      //       moveToSmall(thread_core_big);
      //       moveToBig(thread_core_small);
      //   } else if ((big_min_ipc < B_TH) && (small_max_ipc < S_TH)) { //1200
      //       moveToSmall(thread_core_big);
      //  } else if ((small_max_ipc > S_TH)) { //1200
      //       moveToBig(thread_core_small);
      // }
      
      if ((big_min_ipc < mean[big_core]) && (small_max_ipc > mean[small_core])) { //|| (core_info_list[1].ipc > 1500)) { 2000
            printf("%d, %d ipc_mean: %f, %f\n", thread_core_big, thread_core_small, mean[big_core], mean[small_core]);
            moveToSmall(thread_core_big);
            moveToBig(thread_core_small);
      } else if ((big_min_ipc < mean[big_core]) && (small_max_ipc < mean[small_core])) { //1200
            moveToSmall(thread_core_big);
      }

      m_last_reshuffle = time;
      print_state = true;
   }

   // Call periodic() in parent class
   SchedulerPinnedBase::periodic(time);

   if (print_state && m_debug_output)
         printState();
}

void SchedulerBigSmall::switch_thread(thread_id_t thread_id1, thread_id_t thread_id2)
{
   threadSetAffinity(INVALID_THREAD_ID, thread_id1, sizeof(m_mask_big), &m_mask_big);
   threadSetAffinity(INVALID_THREAD_ID, thread_id2, sizeof(m_mask_small), &m_mask_small);
}

void SchedulerBigSmall::moveToSmall(thread_id_t thread_id)
{
   if (thread_id == INVALID_THREAD_ID) return ;
   printf("Moving thread %d from big core to small.\n", thread_id);
   threadSetAffinity(INVALID_THREAD_ID, thread_id, sizeof(m_mask_small), &m_mask_small);
   m_thread_isbig[thread_id] = false;
}

void SchedulerBigSmall::moveToBig(thread_id_t thread_id)
{
   if (thread_id == INVALID_THREAD_ID) return ;
   printf("Moving thread %d from small core to big.\n", thread_id);
   threadSetAffinity(INVALID_THREAD_ID, thread_id, sizeof(m_mask_big), &m_mask_big);
   m_thread_isbig[thread_id] = true;
}

void SchedulerBigSmall::pickBigThread()
{
   // Randomly select one thread to promote from the small to the big core pool

   // First build a list of all eligible cores
   std::vector<thread_id_t> eligible;
   for(thread_id_t thread_id = 0; thread_id < (thread_id_t)Sim()->getThreadManager()->getNumThreads(); ++thread_id)
   {
      if (m_thread_isbig[thread_id] == false && m_thread_info[thread_id].isRunning())
      {
         eligible.push_back(thread_id);
      }
   }

   if (eligible.size() > 0)
   {
      // Randomly select a thread from our list
      thread_id_t thread_id = eligible[rng_next(m_rng) % eligible.size()];
      moveToBig(thread_id);

      if (m_debug_output)
         std::cout << "[SchedulerBigSmall] thread " << thread_id << " promoted to big core" << std::endl;
   }
}

void SchedulerBigSmall::get_core_ipc(SubsecondTime time, core_id_t coreId)
{

   if (!core_info_list[coreId].s_time) {
      core_info_list[coreId].s_time = Sim()->getStatsManager()->getMetricObject("performance_model", coreId, "elapsed_time");
      core_info_list[coreId].s_instructions = Sim()->getStatsManager()->getMetricObject("performance_model", coreId, "instruction_count");
      core_info_list[coreId].clock = (ComponentPeriod*) Sim()->getDvfsManager()->getCoreDomain(coreId);
      LOG_ASSERT_ERROR(core_info_list[coreId].s_time && core_info_list[coreId].s_instructions && core_info_list[coreId].clock, "Could not find stats / dvfs domain for core 0");
   } else {
      UInt64 d_instructions = core_info_list[coreId].s_instructions->recordMetric() - core_info_list[coreId].l_instructions;
      UInt64 d_time = core_info_list[coreId].s_time->recordMetric() - core_info_list[coreId].l_time;
      UInt64 d_cycles = SubsecondTime::divideRounded(SubsecondTime::FS(d_time), *(core_info_list[coreId].clock));
      if (d_cycles) {
         core_info_list[coreId].ipc = (d_instructions*1000)/d_cycles;
      } 
   }

   core_info_list[coreId].l_time = core_info_list[coreId].s_time->recordMetric();
   core_info_list[coreId].l_instructions = core_info_list[coreId].s_instructions->recordMetric();
}

core_id_t SchedulerBigSmall::getNextCore(core_id_t core_id)
{
   while(true)
   {
      core_id += 1;
      if (core_id >= (core_id_t)Sim()->getConfig()->getApplicationCores())
      {
         core_id %= Sim()->getConfig()->getApplicationCores();
         core_id += 1;
      }
      
      return core_id;
   }
}

core_id_t SchedulerBigSmall::getFreeCore(core_id_t core_first)
{
   core_id_t core_next = core_first;

   do
   {
      if (m_core_thread_running[core_next] == INVALID_THREAD_ID)
         return core_next;

      core_next = getNextCore(core_next);
   }
   while(core_next != core_first);

   return core_first;
}