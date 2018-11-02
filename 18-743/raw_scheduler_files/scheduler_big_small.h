#ifndef __SCHEDULER_BIG_SMALL_H
#define __SCHEDULER_BIG_SMALL_H

#include "scheduler_pinned_base.h"
#include "stats.h"
#include "fixed_point.h"

#include <unordered_map>

class SchedulerBigSmall : public SchedulerPinnedBase
{
   public:
      SchedulerBigSmall(ThreadManager *thread_manager);

      virtual void threadSetInitialAffinity(thread_id_t thread_id);
      virtual void threadStall(thread_id_t thread_id, ThreadManager::stall_type_t reason, SubsecondTime time);
      virtual void threadExit(thread_id_t thread_id, SubsecondTime time);
      virtual void periodic(SubsecondTime time);
      virtual void get_core_ipc(SubsecondTime time, core_id_t coreId);

   private:
      const bool m_debug_output;

      core_id_t getNextCore(core_id_t core_first);
      core_id_t getFreeCore(core_id_t core_first);

      core_id_t m_next_core;

      typedef struct {
         StatsMetricBase *s_time;
         UInt64 l_time;
         StatsMetricBase *s_instructions;
         UInt64 l_instructions;
         ComponentPeriod *clock;
         int ipc;
      } core_info;

      // Configuration
      UInt64 m_num_big_cores;
      cpu_set_t m_mask_big;
      cpu_set_t m_mask_small;
      std::vector<core_info> core_info_list;

      SubsecondTime m_last_reshuffle;
      UInt64 m_rng;
      std::unordered_map<thread_id_t, bool> m_thread_isbig;

      void moveToBig(thread_id_t thread_id);
      void moveToSmall(thread_id_t thread_id);
      void switch_thread(thread_id_t thread_id1, thread_id_t thread_id2);
      void pickBigThread();
};

#endif // __SCHEDULER_BIG_SMALL_H