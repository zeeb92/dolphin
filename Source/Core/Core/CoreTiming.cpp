// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <algorithm>
#include <cinttypes>
#include <mutex>
#include <string>
#include <vector>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/FifoQueue.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"

#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/PowerPC/PowerPC.h"

#include "VideoCommon/Fifo.h"
#include "VideoCommon/VideoBackendBase.h"

namespace CoreTiming
{
struct EventType
{
  TimedCallback callback;
  std::string name;
};

static std::vector<EventType> s_event_types;

struct Event
{
  s64 time;
  u64 userdata;
  int type;
};

constexpr bool operator>(const Event& left, const Event& right)
{
  return left.time > right.time;
}
constexpr bool operator<(const Event& left, const Event& right)
{
  return left.time < right.time;
}

// STATE_TO_SAVE
// The queue is a min-heap using std::make_heap/push_heap/pop_heap.
// We don't use std::priority_queue because we need to be able to serialize, unserialize and
// erase arbitrary events (RemoveEvent()) regardless of the queue order. These aren't accomodated
// by the standard adaptor class.
static std::vector<Event> s_event_queue;
static std::mutex s_ts_write_lock;
static Common::FifoQueue<Event, false> s_ts_queue;

static float s_last_OC_factor;
float g_last_OC_factor_inverted;
int g_slice_length;
static constexpr int MAX_SLICE_LENGTH = 20000;

static s64 s_idled_cycles;
static u32 s_fake_dec_start_value;
static u64 s_fake_dec_start_ticks;

// Are we in a function that has been called from Advance()
static bool s_is_global_timer_sane;

s64 g_global_timer;
u64 g_fake_TB_start_value;
u64 g_fake_TB_start_ticks;

static int s_ev_lost;

static void EmptyTimedCallback(u64 userdata, s64 cyclesLate)
{
}

// Changing the CPU speed in Dolphin isn't actually done by changing the physical clock rate,
// but by changing the amount of work done in a particular amount of time. This tends to be more
// compatible because it stops the games from actually knowing directly that the clock rate has
// changed, and ensures that anything based on waiting a specific number of cycles still works.
//
// Technically it might be more accurate to call this changing the IPC instead of the CPU speed,
// but the effect is largely the same.
static int DowncountToCycles(int downcount)
{
  return static_cast<int>(downcount * g_last_OC_factor_inverted);
}

static int CyclesToDowncount(int cycles)
{
  return static_cast<int>(cycles * s_last_OC_factor);
}

int RegisterEvent(const std::string& name, TimedCallback callback)
{
  EventType type;
  type.name = name;
  type.callback = callback;

  // check for existing type with same name.
  // we want event type names to remain unique so that we can use them for serialization.
  for (auto& event_type : s_event_types)
  {
    if (name == event_type.name)
    {
      WARN_LOG(
          POWERPC,
          "Discarded old event type \"%s\" because a new type with the same name was registered.",
          name.c_str());
      // we don't know if someone might be holding on to the type index,
      // so we gut the old event type instead of actually removing it.
      event_type.name = "_discarded_event";
      event_type.callback = &EmptyTimedCallback;
    }
  }

  s_event_types.push_back(type);
  return static_cast<int>(s_event_types.size() - 1);
}

void UnregisterAllEvents()
{
  _assert_msg_(POWERPC, s_event_queue.empty(), "Cannot unregister events with events pending");
  s_event_types.clear();
  s_event_types.shrink_to_fit();
}

void Init()
{
  s_last_OC_factor = SConfig::GetInstance().m_OCEnable ? SConfig::GetInstance().m_OCFactor : 1.0f;
  g_last_OC_factor_inverted = 1.0f / s_last_OC_factor;
  PowerPC::ppcState.downcount = CyclesToDowncount(MAX_SLICE_LENGTH);
  g_slice_length = MAX_SLICE_LENGTH;
  g_global_timer = 0;
  s_idled_cycles = 0;
  s_is_global_timer_sane = true;

  s_ev_lost = RegisterEvent("_lost_event", &EmptyTimedCallback);
}

void Shutdown()
{
  std::lock_guard<std::mutex> lk(s_ts_write_lock);
  MoveEvents();
  ClearPendingEvents();
  UnregisterAllEvents();
}

void DoState(PointerWrap& p)
{
  std::lock_guard<std::mutex> lk(s_ts_write_lock);
  p.Do(g_slice_length);
  p.Do(g_global_timer);
  p.Do(s_idled_cycles);
  p.Do(s_fake_dec_start_value);
  p.Do(s_fake_dec_start_ticks);
  p.Do(g_fake_TB_start_value);
  p.Do(g_fake_TB_start_ticks);
  p.Do(s_last_OC_factor);
  g_last_OC_factor_inverted = 1.0f / s_last_OC_factor;

  p.DoMarker("CoreTimingData");

  MoveEvents();
  p.DoEachElement(s_event_queue, [](PointerWrap& pw, Event& ev) {
    pw.Do(ev.time);

    // this is why we can't have (nice things) pointers as userdata
    pw.Do(ev.userdata);

    // we can't savestate ev.type directly because events might not get registered in the same
    // order (or at all) every time.
    // so, we savestate the event's type's name, and derive ev.type from that when loading.
    std::string name;
    if (pw.GetMode() != PointerWrap::MODE_READ)
      name = s_event_types[ev.type].name;

    pw.Do(name);
    if (pw.GetMode() == PointerWrap::MODE_READ)
    {
      auto itr = std::find_if(s_event_types.begin(), s_event_types.end(),
                              [&](const EventType& evt) { return evt.name == name; });
      if (itr != s_event_types.end())
      {
        ev.type = static_cast<int>(itr - s_event_types.begin());
      }
      else
      {
        WARN_LOG(POWERPC,
                 "Lost event from savestate because its type, \"%s\", has not been registered.",
                 name.c_str());
        ev.type = s_ev_lost;
      }
    }
  });
  p.DoMarker("CoreTimingEvents");

  // When loading from a save state, we must assume the Event order is random and meaningless.
  // The exact layout of the heap in memory is implementation defined, therefore it is platform
  // and library version specific.
  if (p.GetMode() == PointerWrap::MODE_READ)
    std::make_heap(s_event_queue.begin(), s_event_queue.end(), std::greater<Event>());
}

// This should only be called from the CPU thread. If you are calling
// it from any other thread, you are doing something evil
u64 GetTicks()
{
  u64 ticks = static_cast<u64>(g_global_timer);
  if (!s_is_global_timer_sane)
  {
    int downcount = DowncountToCycles(PowerPC::ppcState.downcount);
    ticks += g_slice_length - downcount;
  }
  return ticks;
}

u64 GetIdleTicks()
{
  return static_cast<u64>(s_idled_cycles);
}

void ClearPendingEvents()
{
  s_event_queue.clear();
}

void ScheduleEvent(s64 cycles_into_future, int event_type, u64 userdata, FromThread from)
{
  bool from_cpu_thread;
  if (from == FromThread::ANY)
  {
    from_cpu_thread = Core::IsCPUThread();
  }
  else
  {
    from_cpu_thread = from == FromThread::CPU;
    _assert_msg_(POWERPC, from_cpu_thread == Core::IsCPUThread(),
                 "ScheduleEvent from wrong thread (%s)", from_cpu_thread ? "CPU" : "non-CPU");
  }

  if (from_cpu_thread)
  {
    s64 timeout = GetTicks() + cycles_into_future;

    // If this event needs to be scheduled before the next advance(), force one early
    if (!s_is_global_timer_sane)
      ForceExceptionCheck(cycles_into_future);

    s_event_queue.emplace_back(Event{timeout, userdata, event_type});
    std::push_heap(s_event_queue.begin(), s_event_queue.end(), std::greater<Event>());
  }
  else
  {
    if (Core::g_want_determinism)
    {
      ERROR_LOG(POWERPC, "Someone scheduled an off-thread \"%s\" event while netplay or "
                         "movie play/record was active.  This is likely to cause a desync.",
                s_event_types[event_type].name.c_str());
    }

    std::lock_guard<std::mutex> lk(s_ts_write_lock);
    s_ts_queue.Push(Event{g_global_timer + cycles_into_future, userdata, event_type});
  }
}

void RemoveEvent(int event_type)
{
  auto itr = std::remove_if(s_event_queue.begin(), s_event_queue.end(),
                            [&](const Event& e) { return e.type == event_type; });

  // Removing random items breaks the invariant so we have to re-establish it.
  if (itr != s_event_queue.end())
  {
    s_event_queue.erase(itr, s_event_queue.end());
    std::make_heap(s_event_queue.begin(), s_event_queue.end(), std::greater<Event>());
  }
}

void RemoveAllEvents(int event_type)
{
  MoveEvents();
  RemoveEvent(event_type);
}

void ForceExceptionCheck(s64 cycles)
{
  if (DowncountToCycles(PowerPC::ppcState.downcount) > cycles)
  {
    // downcount is always (much) smaller than MAX_INT so we can safely cast cycles to an int here.
    // Account for cycles already executed by adjusting the g_slice_length
    g_slice_length -= (DowncountToCycles(PowerPC::ppcState.downcount) - static_cast<int>(cycles));
    PowerPC::ppcState.downcount = CyclesToDowncount(static_cast<int>(cycles));
  }
}

void MoveEvents()
{
  for (Event ev; s_ts_queue.Pop(ev);)
  {
    s_event_queue.emplace_back(std::move(ev));
    std::push_heap(s_event_queue.begin(), s_event_queue.end(), std::greater<Event>());
  }
}

void Advance()
{
  MoveEvents();

  int cyclesExecuted = g_slice_length - DowncountToCycles(PowerPC::ppcState.downcount);
  g_global_timer += cyclesExecuted;
  s_last_OC_factor = SConfig::GetInstance().m_OCEnable ? SConfig::GetInstance().m_OCFactor : 1.0f;
  g_last_OC_factor_inverted = 1.0f / s_last_OC_factor;
  g_slice_length = MAX_SLICE_LENGTH;

  s_is_global_timer_sane = true;

  while (!s_event_queue.empty() && s_event_queue.front().time <= g_global_timer)
  {
    Event evt = std::move(s_event_queue.front());
    std::pop_heap(s_event_queue.begin(), s_event_queue.end(), std::greater<Event>());
    s_event_queue.pop_back();
    // NOTICE_LOG(POWERPC, "[Scheduler] %-20s (%lld, %lld)", evt.type->name->c_str(),
    //            g_global_timer, evt.time);
    s_event_types[evt.type].callback(evt.userdata, g_global_timer - evt.time);
  }

  s_is_global_timer_sane = false;

  // Still events left (scheduled in the future)
  if (!s_event_queue.empty())
  {
    g_slice_length = static_cast<int>(
        std::min<s64>(s_event_queue.front().time - g_global_timer, MAX_SLICE_LENGTH));
  }

  PowerPC::ppcState.downcount = CyclesToDowncount(g_slice_length);

  // Check for any external exceptions.
  // It's important to do this after processing events otherwise any exceptions will be delayed
  // until the next slice:
  //        Pokemon Box refuses to boot if the first exception from the audio DMA is received late
  PowerPC::CheckExternalExceptions();
}

void LogPendingEvents()
{
  auto clone = s_event_queue;
  std::sort(clone.begin(), clone.end());
  for (const Event& ev : clone)
  {
    INFO_LOG(POWERPC, "PENDING: Now: %" PRId64 " Pending: %" PRId64 " Type: %s (%d)",
             g_global_timer, ev.time,
             ev.type < s_event_types.size() ? s_event_types[ev.type].name.c_str() : "<INVALID>",
             ev.type);
  }
}

void Idle()
{
  if (SConfig::GetInstance().bSyncGPUOnSkipIdleHack)
  {
    // When the FIFO is processing data we must not advance because in this way
    // the VI will be desynchronized. So, We are waiting until the FIFO finish and
    // while we process only the events required by the FIFO.
    Fifo::FlushGpu();
  }

  s_idled_cycles += DowncountToCycles(PowerPC::ppcState.downcount);
  PowerPC::ppcState.downcount = 0;
}

std::string GetScheduledEventsSummary()
{
  std::string text = "Scheduled events\n";
  text.reserve(1000);

  auto clone = s_event_queue;
  std::sort(clone.begin(), clone.end());
  for (const Event& ev : clone)
  {
    unsigned int t = ev.type;
    if (t >= s_event_types.size())
    {
      PanicAlertT("Invalid event type %i", t);
      continue;
    }

    const std::string& name = s_event_types[ev.type].name;

    text +=
        StringFromFormat("%s : %" PRIi64 " %016" PRIx64 "\n", name.c_str(), ev.time, ev.userdata);
  }
  return text;
}

u32 GetFakeDecStartValue()
{
  return s_fake_dec_start_value;
}

void SetFakeDecStartValue(u32 val)
{
  s_fake_dec_start_value = val;
}

u64 GetFakeDecStartTicks()
{
  return s_fake_dec_start_ticks;
}

void SetFakeDecStartTicks(u64 val)
{
  s_fake_dec_start_ticks = val;
}

u64 GetFakeTBStartValue()
{
  return g_fake_TB_start_value;
}

void SetFakeTBStartValue(u64 val)
{
  g_fake_TB_start_value = val;
}

u64 GetFakeTBStartTicks()
{
  return g_fake_TB_start_ticks;
}

void SetFakeTBStartTicks(u64 val)
{
  g_fake_TB_start_ticks = val;
}

}  // namespace
