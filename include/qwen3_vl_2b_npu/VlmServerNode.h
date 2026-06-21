#ifndef VLMSERVERNODE_H_
#define VLMSERVERNODE_H_

#include <string>
#include <mutex>

#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <sensor_msgs/msg/image.hpp>
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
    // ─── 订阅回调：摄像头帧 → cv::Mat ───
    void imageCallback(const sensor_msgs::msg::Image::ConstSharedPtr& msg);

    // ─── Action 回调 ───
    rclcpp_action::GoalResponse handle_goal(
        const rclcpp_action::GoalUUID& uuid,
        std::shared_ptr<const StreamQuery::Goal> goal);

    rclcpp_action::CancelResponse handle_cancel(
        std::shared_ptr<GoalHandle> goal_handle);

    void handle_accepted(std::shared_ptr<GoalHandle> goal_handle);

    /// @brief 实际推理（在独立线程运行）
    void execute(std::shared_ptr<GoalHandle> goal_handle);

    // ─── 核心成员 ───
    RK35llm     vlm_;
    cv::Mat     latest_frame_;
    rclcpp::Time last_frame_stamp_;
    std::mutex  frame_mtx_;

    // ─── ROS2 句柄 ───
    rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr img_sub_;
    rclcpp_action::Server<StreamQuery>::SharedPtr action_srv_;
};

}  // namespace qwen3_vl_2b_npu

#endif  // VLMSERVERNODE_H_
