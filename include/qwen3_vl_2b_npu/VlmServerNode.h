#ifndef VLMSERVERNODE_H_
#define VLMSERVERNODE_H_

#include <string>
#include <mutex>
#include <atomic>
#include <vector>

#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <std_msgs/msg/string.hpp>
#include <opencv2/core/mat.hpp>

#include <qwen3_vl_2b_npu/action/stream_query.hpp>
#include "qwen3_vl_2b_npu/RK35llm.h"

namespace qwen3_vl_2b_npu
{

class VlmServerNode : public rclcpp::Node
{
public:
    using StreamQuery = qwen3_vl_2b_npu::action::StreamQuery;
    using GoalHandle  = rclcpp_action::ServerGoalHandle<StreamQuery>;

    explicit VlmServerNode();
    ~VlmServerNode() override = default;

private:
    // ─── 订阅 ──────────────────────────────────────────────
    void imageCallback(const sensor_msgs::msg::Image::ConstSharedPtr& msg);
    void asrCallback(const std_msgs::msg::String::ConstSharedPtr& msg);

    // ─── Action 回调 ───────────────────────────────────────
    rclcpp_action::GoalResponse handle_goal(
        const rclcpp_action::GoalUUID& uuid,
        std::shared_ptr<const StreamQuery::Goal> goal);

    rclcpp_action::CancelResponse handle_cancel(
        std::shared_ptr<GoalHandle> goal_handle);

    void handle_accepted(std::shared_ptr<GoalHandle> goal_handle);

    /// @brief Action 内部推理（薄封装，调 process_query）
    void execute(std::shared_ptr<GoalHandle> goal_handle);

    // ─── 核心推理入口（ASR 和 Action 共用）───────────────
    /// @param question     原始问题（可能含触发词，已由 asrCallback 预处理过）
    /// @param needs_image  是否编码图像
    /// @param goal_handle  Action 句柄（nullptr = ASR 来源）
    void process_query(
        const std::string& question,
        bool needs_image,
        std::shared_ptr<GoalHandle> goal_handle);

    // ─── 触发词匹配 ───────────────────────────────────────
    /// @return 匹配到的触发词字符串；未命中返回空
    std::string match_trigger_word(const std::string& text);

    /// @return 匹配到的退出词字符串；未命中返回空
    std::string match_exit_word(const std::string& text);

    // ─── 核心成员 ─────────────────────────────────────────
    RK35llm     vlm_;
    cv::Mat     latest_frame_;
    rclcpp::Time last_frame_stamp_;
    std::mutex  frame_mtx_;

    // 图像模式
    std::atomic<bool> is_image_mode_{true};    // 默认开启看图
    rclcpp::Time      last_image_time_;
    double            image_mode_timeout_s_ = 60.0;
    double            frame_timeout_s_      = 5.0;

    // 触发词/退出词 列表
    std::vector<std::string> trigger_words_;
    std::vector<std::string> exit_words_;

    // 安全时间减法（时钟源不一致时返回 0 不崩溃）
    double safe_seconds_diff(const rclcpp::Time& a, const rclcpp::Time& b);

    // NPU 互斥锁（同一时刻只有一个推理在跑）
    std::mutex npu_mutex_;

    // ─── ROS2 句柄 ────────────────────────────────────────
    rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr img_sub_;
    rclcpp::Subscription<std_msgs::msg::String>::SharedPtr   asr_sub_;
    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr      pub_result_;
    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr      pub_stream_;
    rclcpp_action::Server<StreamQuery>::SharedPtr            action_srv_;
};

}  // namespace qwen3_vl_2b_npu

#endif  // VLMSERVERNODE_H_
