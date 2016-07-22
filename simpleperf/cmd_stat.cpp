/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>

#include <algorithm>
#include <chrono>
#include <set>
#include <string>
#include <vector>

#include <android-base/logging.h>
#include <android-base/strings.h>

#include "command.h"
#include "environment.h"
#include "event_attr.h"
#include "event_fd.h"
#include "event_selection_set.h"
#include "event_type.h"
#include "scoped_signal_handler.h"
#include "utils.h"
#include "workload.h"

namespace {

static std::vector<std::string> default_measured_event_types{
    "cpu-cycles",   "stalled-cycles-frontend", "stalled-cycles-backend",
    "instructions", "branch-instructions",     "branch-misses",
    "task-clock",   "context-switches",        "page-faults",
};

static volatile bool signaled;
static void signal_handler(int) { signaled = true; }

struct CounterSummary {
  std::string type_name;
  std::string modifier;
  uint32_t group_id;
  uint64_t count;
  double scale;
  std::string readable_count;
  std::string comment;
  bool auto_generated;

  CounterSummary(const std::string& type_name, const std::string& modifier,
                 uint32_t group_id, uint64_t count, double scale,
                 bool auto_generated, bool csv)
      : type_name(type_name),
        modifier(modifier),
        group_id(group_id),
        count(count),
        scale(scale),
        auto_generated(auto_generated) {
    readable_count = ReadableCountValue(csv);
  }

  bool IsMonitoredAtTheSameTime(const CounterSummary& other) const {
    // Two summaries are monitored at the same time if they are in the same
    // group or are monitored all the time.
    if (group_id == other.group_id) {
      return true;
    }
    return IsMonitoredAllTheTime() && other.IsMonitoredAllTheTime();
  }

  std::string Name() const {
    if (modifier.empty()) {
      return type_name;
    }
    return type_name + ":" + modifier;
  }

 private:
  std::string ReadableCountValue(bool csv) {
    if (type_name == "cpu-clock" || type_name == "task-clock") {
      // Convert nanoseconds to milliseconds.
      double value = count / 1e6;
      return android::base::StringPrintf("%lf(ms)", value);
    } else {
      // Convert big numbers to human friendly mode. For example,
      // 1000000 will be converted to 1,000,000.
      std::string s = android::base::StringPrintf("%" PRIu64, count);
      if (csv) {
        return s;
      } else {
        for (size_t i = s.size() - 1, j = 1; i > 0; --i, ++j) {
          if (j == 3) {
            s.insert(s.begin() + i, ',');
            j = 0;
          }
        }
        return s;
      }
    }
  }

  bool IsMonitoredAllTheTime() const {
    // If an event runs all the time it is enabled (by not sharing hardware
    // counters with other events), the scale of its summary is usually within
    // [1, 1 + 1e-5]. By setting SCALE_ERROR_LIMIT to 1e-5, We can identify
    // events monitored all the time in most cases while keeping the report
    // error rate <= 1e-5.
    constexpr double SCALE_ERROR_LIMIT = 1e-5;
    return (fabs(scale - 1.0) < SCALE_ERROR_LIMIT);
  }
};

class CounterSummaries {
 public:
  explicit CounterSummaries(bool csv) : csv_(csv) {}
  void AddSummary(const CounterSummary& summary) {
    summaries_.push_back(summary);
  }

  const CounterSummary* FindSummary(const std::string& type_name,
                                    const std::string& modifier) {
    for (const auto& s : summaries_) {
      if (s.type_name == type_name && s.modifier == modifier) {
        return &s;
      }
    }
    return nullptr;
  }

  // If we have two summaries monitoring the same event type at the same time,
  // that one is for user space only, and the other is for kernel space only;
  // then we can automatically generate a summary combining the two results.
  // For example, a summary of branch-misses:u and a summary for branch-misses:k
  // can generate a summary of branch-misses.
  void AutoGenerateSummaries() {
    for (size_t i = 0; i < summaries_.size(); ++i) {
      const CounterSummary& s = summaries_[i];
      if (s.modifier == "u") {
        const CounterSummary* other = FindSummary(s.type_name, "k");
        if (other != nullptr && other->IsMonitoredAtTheSameTime(s)) {
          if (FindSummary(s.type_name, "") == nullptr) {
            AddSummary(CounterSummary(s.type_name, "", s.group_id,
                                      s.count + other->count, s.scale, true,
                                      csv_));
          }
        }
      }
    }
  }

  void GenerateComments(double duration_in_sec) {
    for (auto& s : summaries_) {
      s.comment = GetCommentForSummary(s, duration_in_sec);
    }
  }

  void Show(FILE* fp) {
    size_t count_column_width = 0;
    size_t name_column_width = 0;
    size_t comment_column_width = 0;
    for (auto& s : summaries_) {
      count_column_width =
          std::max(count_column_width, s.readable_count.size());
      name_column_width = std::max(name_column_width, s.Name().size());
      comment_column_width = std::max(comment_column_width, s.comment.size());
    }

    for (auto& s : summaries_) {
      if (csv_) {
        fprintf(fp, "%s,%s,%s,(%.0lf%%)%s\n", s.readable_count.c_str(),
                s.Name().c_str(), s.comment.c_str(), 1.0 / s.scale * 100,
                (s.auto_generated ? " (generated)," : ","));
      } else {
        fprintf(fp, "  %*s  %-*s   # %-*s  (%.0lf%%)%s\n",
                static_cast<int>(count_column_width), s.readable_count.c_str(),
                static_cast<int>(name_column_width), s.Name().c_str(),
                static_cast<int>(comment_column_width), s.comment.c_str(),
                1.0 / s.scale * 100, (s.auto_generated ? " (generated)" : ""));
      }
    }
  }

 private:
  std::string GetCommentForSummary(const CounterSummary& s,
                                   double duration_in_sec) {
    char sap_mid;
    if (csv_) {
      sap_mid = ',';
    } else {
      sap_mid = ' ';
    }
    if (s.type_name == "task-clock") {
      double run_sec = s.count / 1e9;
      double used_cpus = run_sec / (duration_in_sec / s.scale);
      return android::base::StringPrintf("%lf%ccpus used", used_cpus, sap_mid);
    }
    if (s.type_name == "cpu-clock") {
      return "";
    }
    if (s.type_name == "cpu-cycles") {
      double hz = s.count / (duration_in_sec / s.scale);
      return android::base::StringPrintf("%lf%cGHz", hz / 1e9, sap_mid);
    }
    if (s.type_name == "instructions" && s.count != 0) {
      const CounterSummary* other = FindSummary("cpu-cycles", s.modifier);
      if (other != nullptr && other->IsMonitoredAtTheSameTime(s)) {
        double cpi = static_cast<double>(other->count) / s.count;
        return android::base::StringPrintf("%lf%ccycles per instruction", cpi,
                                           sap_mid);
      }
    }
    if (android::base::EndsWith(s.type_name, "-misses")) {
      std::string other_name;
      if (s.type_name == "cache-misses") {
        other_name = "cache-references";
      } else if (s.type_name == "branch-misses") {
        other_name = "branch-instructions";
      } else {
        other_name =
            s.type_name.substr(0, s.type_name.size() - strlen("-misses")) + "s";
      }
      const CounterSummary* other = FindSummary(other_name, s.modifier);
      if (other != nullptr && other->IsMonitoredAtTheSameTime(s) &&
          other->count != 0) {
        double miss_rate = static_cast<double>(s.count) / other->count;
        return android::base::StringPrintf("%lf%%%cmiss rate", miss_rate * 100,
                                           sap_mid);
      }
    }
    double rate = s.count / (duration_in_sec / s.scale);
    if (rate > 1e9) {
      return android::base::StringPrintf("%.3lf%cG/sec", rate / 1e9, sap_mid);
    }
    if (rate > 1e6) {
      return android::base::StringPrintf("%.3lf%cM/sec", rate / 1e6, sap_mid);
    }
    if (rate > 1e3) {
      return android::base::StringPrintf("%.3lf%cK/sec", rate / 1e3, sap_mid);
    }
    return android::base::StringPrintf("%.3lf%c/sec", rate, sap_mid);
  }

 private:
  std::vector<CounterSummary> summaries_;
  bool csv_;
};

class StatCommand : public Command {
 public:
  StatCommand()
      : Command("stat", "gather performance counter information",
                // clang-format off
"Usage: simpleperf stat [options] [command [command-args]]\n"
"       Gather performance counter information of running [command].\n"
"       And -a/-p/-t option can be used to change target of counter information.\n"
"-a           Collect system-wide information.\n"
"--cpu cpu_item1,cpu_item2,...\n"
"                 Collect information only on the selected cpus. cpu_item can\n"
"                 be a cpu number like 1, or a cpu range like 0-3.\n"
"--csv            Write report in comma separate form.\n"
"--duration time_in_sec  Monitor for time_in_sec seconds instead of running\n"
"                        [command]. Here time_in_sec may be any positive\n"
"                        floating point number.\n"
"-e event1[:modifier1],event2[:modifier2],...\n"
"                 Select the event list to count. Use `simpleperf list` to find\n"
"                 all possible event names. Modifiers can be added to define\n"
"                 how the event should be monitored. Possible modifiers are:\n"
"                   u - monitor user space events only\n"
"                   k - monitor kernel space events only\n"
"--group event1[:modifier],event2[:modifier2],...\n"
"             Similar to -e option. But events specified in the same --group\n"
"             option are monitored as a group, and scheduled in and out at the\n"
"             same time.\n"
"--no-inherit     Don't stat created child threads/processes.\n"
"-o output_filename  Write report to output_filename instead of standard output.\n"
"-p pid1,pid2,... Stat events on existing processes. Mutually exclusive with -a.\n"
"-t tid1,tid2,... Stat events on existing threads. Mutually exclusive with -a.\n"
"--verbose        Show result in verbose mode.\n"
                // clang-format on
                ),
        verbose_mode_(false),
        system_wide_collection_(false),
        child_inherit_(true),
        csv_(false) {
    // Die if parent exits.
    prctl(PR_SET_PDEATHSIG, SIGHUP, 0, 0, 0);
    signaled = false;
    scoped_signal_handler_.reset(
        new ScopedSignalHandler({SIGCHLD, SIGINT, SIGTERM}, signal_handler));
  }

  bool Run(const std::vector<std::string>& args);

 private:
  bool ParseOptions(const std::vector<std::string>& args,
                    std::vector<std::string>* non_option_args);
  bool AddDefaultMeasuredEventTypes();
  void SetEventSelectionFlags();
  bool ShowCounters(const std::vector<CountersInfo>& counters,
                    double duration_in_sec);

  bool verbose_mode_;
  bool system_wide_collection_;
  bool child_inherit_;
  std::vector<pid_t> monitored_threads_;
  std::vector<int> cpus_;
  EventSelectionSet event_selection_set_;
  std::string output_filename_;
  bool csv_;

  std::unique_ptr<ScopedSignalHandler> scoped_signal_handler_;
};

bool StatCommand::Run(const std::vector<std::string>& args) {
  if (!CheckPerfEventLimit()) {
    return false;
  }

  // 1. Parse options, and use default measured event types if not given.
  std::vector<std::string> workload_args;
  if (!ParseOptions(args, &workload_args)) {
    return false;
  }
  if (event_selection_set_.empty()) {
    if (!AddDefaultMeasuredEventTypes()) {
      return false;
    }
  }
  SetEventSelectionFlags();

  // 2. Create workload.
  std::unique_ptr<Workload> workload;
  if (!workload_args.empty()) {
    workload = Workload::CreateWorkload(workload_args);
    if (workload == nullptr) {
      return false;
    }
  }
  if (!system_wide_collection_ && monitored_threads_.empty()) {
    if (workload != nullptr) {
      monitored_threads_.push_back(workload->GetPid());
      event_selection_set_.SetEnableOnExec(true);
    } else {
      LOG(ERROR)
          << "No threads to monitor. Try `simpleperf help stat` for help\n";
      return false;
    }
  }

  // 3. Open perf_event_files.
  if (system_wide_collection_) {
    if (!event_selection_set_.OpenEventFilesForCpus(cpus_)) {
      return false;
    }
  } else {
    if (cpus_.empty()) {
      cpus_ = {-1};
    }
    if (!event_selection_set_.OpenEventFilesForThreadsOnCpus(monitored_threads_,
                                                             cpus_)) {
      return false;
    }
  }

  // 4. Count events while workload running.
  auto start_time = std::chrono::steady_clock::now();
  if (workload != nullptr && !workload->Start()) {
    return false;
  }
  while (!signaled) {
    sleep(1);
  }
  auto end_time = std::chrono::steady_clock::now();

  // 5. Read and print counters.
  std::vector<CountersInfo> counters;
  if (!event_selection_set_.ReadCounters(&counters)) {
    return false;
  }
  double duration_in_sec =
      std::chrono::duration_cast<std::chrono::duration<double>>(end_time -
                                                                start_time)
          .count();
  if (!ShowCounters(counters, duration_in_sec)) {
    return false;
  }
  return true;
}

bool StatCommand::ParseOptions(const std::vector<std::string>& args,
                               std::vector<std::string>* non_option_args) {
  std::set<pid_t> tid_set;
  double duration_in_sec = 0;
  size_t i;
  for (i = 0; i < args.size() && args[i].size() > 0 && args[i][0] == '-'; ++i) {
    if (args[i] == "-a") {
      system_wide_collection_ = true;
    } else if (args[i] == "--cpu") {
      if (!NextArgumentOrError(args, &i)) {
        return false;
      }
      cpus_ = GetCpusFromString(args[i]);
    } else if (args[i] == "--csv") {
      csv_ = true;
    } else if (args[i] == "--duration") {
      if (!NextArgumentOrError(args, &i)) {
        return false;
      }
      errno = 0;
      char* endptr;
      duration_in_sec = strtod(args[i].c_str(), &endptr);
      if (duration_in_sec <= 0 || *endptr != '\0' || errno == ERANGE) {
        LOG(ERROR) << "Invalid duration: " << args[i].c_str();
        return false;
      }
    } else if (args[i] == "-e") {
      if (!NextArgumentOrError(args, &i)) {
        return false;
      }
      std::vector<std::string> event_types = android::base::Split(args[i], ",");
      for (auto& event_type : event_types) {
        if (!event_selection_set_.AddEventType(event_type)) {
          return false;
        }
      }
    } else if (args[i] == "--group") {
      if (!NextArgumentOrError(args, &i)) {
        return false;
      }
      std::vector<std::string> event_types = android::base::Split(args[i], ",");
      if (!event_selection_set_.AddEventGroup(event_types)) {
        return false;
      }
    } else if (args[i] == "--no-inherit") {
      child_inherit_ = false;
    } else if (args[i] == "-o") {
      if (!NextArgumentOrError(args, &i)) {
        return false;
      }
      output_filename_ = args[i];
    } else if (args[i] == "-p") {
      if (!NextArgumentOrError(args, &i)) {
        return false;
      }
      if (!GetValidThreadsFromProcessString(args[i], &tid_set)) {
        return false;
      }
    } else if (args[i] == "-t") {
      if (!NextArgumentOrError(args, &i)) {
        return false;
      }
      if (!GetValidThreadsFromThreadString(args[i], &tid_set)) {
        return false;
      }
    } else if (args[i] == "--verbose") {
      verbose_mode_ = true;
    } else {
      ReportUnknownOption(args, i);
      return false;
    }
  }

  monitored_threads_.insert(monitored_threads_.end(), tid_set.begin(),
                            tid_set.end());
  if (system_wide_collection_ && !monitored_threads_.empty()) {
    LOG(ERROR) << "Stat system wide and existing processes/threads can't be "
                  "used at the same time.";
    return false;
  }
  if (system_wide_collection_ && !IsRoot()) {
    LOG(ERROR) << "System wide profiling needs root privilege.";
    return false;
  }

  non_option_args->clear();
  for (; i < args.size(); ++i) {
    non_option_args->push_back(args[i]);
  }
  if (duration_in_sec != 0) {
    if (!non_option_args->empty()) {
      LOG(ERROR) << "Using --duration option while running a command is not "
                    "supported.";
      return false;
    }
    non_option_args->insert(
        non_option_args->end(),
        {"sleep", android::base::StringPrintf("%f", duration_in_sec)});
  }
  return true;
}

bool StatCommand::AddDefaultMeasuredEventTypes() {
  for (auto& name : default_measured_event_types) {
    // It is not an error when some event types in the default list are not
    // supported by the kernel.
    const EventType* type = FindEventTypeByName(name);
    if (type != nullptr &&
        IsEventAttrSupportedByKernel(CreateDefaultPerfEventAttr(*type))) {
      if (!event_selection_set_.AddEventType(name)) {
        return false;
      }
    }
  }
  if (event_selection_set_.empty()) {
    LOG(ERROR) << "Failed to add any supported default measured types";
    return false;
  }
  return true;
}

void StatCommand::SetEventSelectionFlags() {
  event_selection_set_.SetInherit(child_inherit_);
}

bool StatCommand::ShowCounters(const std::vector<CountersInfo>& counters,
                               double duration_in_sec) {
  std::unique_ptr<FILE, decltype(&fclose)> fp_holder(nullptr, fclose);
  FILE* fp = stdout;
  if (!output_filename_.empty()) {
    fp_holder.reset(fopen(output_filename_.c_str(), "w"));
    if (fp_holder == nullptr) {
      PLOG(ERROR) << "failed to open " << output_filename_;
      return false;
    }
    fp = fp_holder.get();
  }
  if (csv_) {
    fprintf(fp, "Performance counter statistics,\n");
  } else {
    fprintf(fp, "Performance counter statistics:\n\n");
  }

  if (verbose_mode_) {
    for (auto& counters_info : counters) {
      const EventTypeAndModifier& event_type =
          counters_info.selection->event_type_modifier;
      for (auto& counter_info : counters_info.counters) {
        if (csv_) {
          fprintf(fp, "%s,tid,%d,cpu,%d,count,%" PRIu64 ",time_enabled,%" PRIu64
                      ",time running,%" PRIu64 ",id,%" PRIu64 ",\n",
                  event_type.name.c_str(), counter_info.tid, counter_info.cpu,
                  counter_info.counter.value, counter_info.counter.time_enabled,
                  counter_info.counter.time_running, counter_info.counter.id);
        } else {
          fprintf(fp,
                  "%s(tid %d, cpu %d): count %" PRIu64 ", time_enabled %" PRIu64
                  ", time running %" PRIu64 ", id %" PRIu64 "\n",
                  event_type.name.c_str(), counter_info.tid, counter_info.cpu,
                  counter_info.counter.value, counter_info.counter.time_enabled,
                  counter_info.counter.time_running, counter_info.counter.id);
        }
      }
    }
  }

  CounterSummaries summaries(csv_);
  for (auto& counters_info : counters) {
    uint64_t value_sum = 0;
    uint64_t time_enabled_sum = 0;
    uint64_t time_running_sum = 0;
    for (auto& counter_info : counters_info.counters) {
      // If time_running is 0, the program has never run on this event and we
      // shouldn't summarize it.
      if (counter_info.counter.time_running != 0) {
        value_sum += counter_info.counter.value;
        time_enabled_sum += counter_info.counter.time_enabled;
        time_running_sum += counter_info.counter.time_running;
      }
    }
    double scale = 1.0;
    if (time_running_sum < time_enabled_sum && time_running_sum != 0) {
      scale = static_cast<double>(time_enabled_sum) / time_running_sum;
    }
    summaries.AddSummary(CounterSummary(
        counters_info.selection->event_type_modifier.event_type.name,
        counters_info.selection->event_type_modifier.modifier,
        counters_info.selection->group_id, value_sum, scale, false, csv_));
  }
  summaries.AutoGenerateSummaries();
  summaries.GenerateComments(duration_in_sec);
  summaries.Show(fp);

  if (csv_)
    fprintf(fp, "Total test time,%lf,seconds,\n", duration_in_sec);
  else
    fprintf(fp, "\nTotal test time: %lf seconds.\n", duration_in_sec);
  return true;
}

}  // namespace

void RegisterStatCommand() {
  RegisterCommand("stat",
                  [] { return std::unique_ptr<Command>(new StatCommand); });
}
