#include "policy.h"
#include <queue>
#include <unordered_map>
#include <vector>
#include <algorithm>

// 任务状态结构体
struct TaskState {
    Event::Task task;
    bool is_io_active;  // 是否正在进行 I/O 操作
    int remaining_time; // 剩余执行时间
    int slack_time;     // 松弛时间 = 截止时间 - 当前时间 - 剩余时间
    int io_remaining;   // I/O 剩余时间
    int urgency_level;  // 紧急程度：0-不紧急，1-较紧急，2-非常紧急
};

// 全局任务状态表
std::unordered_map<int, TaskState> task_states;

// 比较函数：优先考虑紧急程度高的任务
bool compare_urgency(const Event::Task& a, const Event::Task& b) {
    int urgency_a = task_states[a.taskId].urgency_level;
    int urgency_b = task_states[b.taskId].urgency_level;
    if (urgency_a != urgency_b) {
        return urgency_a > urgency_b;
    }
    return task_states[a.taskId].slack_time < task_states[b.taskId].slack_time;
}

// 比较函数：优先考虑高优先级且紧急的任务
bool compare_priority_urgency(const Event::Task& a, const Event::Task& b) {
    if (a.priority != b.priority) {
        return a.priority == Event::Task::Priority::kHigh;
    }
    return compare_urgency(a, b);
}

// 更新所有任务的松弛时间和紧急程度
void update_task_states(int current_time) {
    for (auto& [taskId, state] : task_states) {
        if (!state.is_io_active) {
            state.slack_time = state.task.deadline - current_time - state.remaining_time;
            
            // 更新紧急程度
            if (state.slack_time <= 0) {
                state.urgency_level = 2;  // 非常紧急
            } else if (state.slack_time <= 5) {
                state.urgency_level = 1;  // 较紧急
            } else {
                state.urgency_level = 0;  // 不紧急
            }
            
            // 高优先级任务更早进入紧急状态
            if (state.task.priority == Event::Task::Priority::kHigh) {
                if (state.slack_time <= 10) {
                    state.urgency_level = std::max(state.urgency_level, 1);
                }
            }
        }
    }
}

Action policy(const std::vector<Event>& events, int current_cpu,
              int current_io) {
    Action action;
    action.cpuTask = 0;  // 默认 CPU 空闲
    action.ioTask = 0;   // 默认 I/O 空闲

    // 处理所有事件
    for (const auto& event : events) {
        switch (event.type) {
            case Event::Type::kTaskArrival:
                // 新任务到达，初始化任务状态
                task_states[event.task.taskId] = {
                    event.task,
                    false,
                    event.task.deadline - event.time,  // 初始剩余时间
                    event.task.deadline - event.time,  // 初始松弛时间
                    0,  // 初始 I/O 剩余时间
                    0   // 初始紧急程度
                };
                break;

            case Event::Type::kTaskFinish:
                // 任务完成，从状态表中移除
                task_states.erase(event.task.taskId);
                break;

            case Event::Type::kIoRequest:
                // I/O 请求，更新任务状态
                if (task_states.count(event.task.taskId)) {
                    task_states[event.task.taskId].is_io_active = true;
                    // 估计 I/O 时间
                    task_states[event.task.taskId].io_remaining = 10;  // 假设 I/O 需要10个单位时间
                }
                break;

            case Event::Type::kIoEnd:
                // I/O 完成，更新任务状态
                if (task_states.count(event.task.taskId)) {
                    task_states[event.task.taskId].is_io_active = false;
                    task_states[event.task.taskId].io_remaining = 0;
                }
                break;

            case Event::Type::kTimer:
                // 时钟中断，更新所有任务状态
                update_task_states(event.time);
                for (auto& [taskId, state] : task_states) {
                    if (state.is_io_active && state.io_remaining > 0) {
                        state.io_remaining--;
                    }
                }
                break;

            default:
                break;
        }
    }

    // 准备就绪队列
    std::vector<Event::Task> ready_tasks;
    for (const auto& [taskId, state] : task_states) {
        if (!state.is_io_active) {  // 不在进行 I/O 操作的任务
            ready_tasks.push_back(state.task);
        }
    }

    // 按优先级和紧急程度排序
    std::sort(ready_tasks.begin(), ready_tasks.end(), compare_priority_urgency);

    // 选择 CPU 任务
    if (!ready_tasks.empty()) {
        action.cpuTask = ready_tasks[0].taskId;
    }

    // 选择 I/O 任务
    // 只有当当前 I/O 任务完成时才能选择新的 I/O 任务
    if (current_io == 0) {
        // 优先选择高优先级且 I/O 剩余时间短的任务
        std::vector<Event::Task> io_tasks;
        for (const auto& [taskId, state] : task_states) {
            if (state.is_io_active && taskId != current_cpu) {  // 确保不选择当前 CPU 任务
                io_tasks.push_back(state.task);
            }
        }
        // 按优先级和 I/O 剩余时间排序
        std::sort(io_tasks.begin(), io_tasks.end(), [](const Event::Task& a, const Event::Task& b) {
            if (a.priority != b.priority) {
                return a.priority == Event::Task::Priority::kHigh;
            }
            return task_states[a.taskId].io_remaining < task_states[b.taskId].io_remaining;
        });
        if (!io_tasks.empty()) {
            action.ioTask = io_tasks[0].taskId;
        }
    } else {
        // 如果当前有 I/O 任务，继续执行
        action.ioTask = current_io;
    }

    return action;
}
