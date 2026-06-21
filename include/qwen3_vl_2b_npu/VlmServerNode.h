#ifndef VLMSERVERNODE_H_
#define VLMSERVERNODE_H_

#include <string>
#include <mutex>

#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <opencv2/core/mat.hpp>    // cv::Mat 前向声明即可，避免拉入整个 OpenCV

#include <qwen3_vl_2b_npu/srv/vlm_query.hpp>
#include "qwen3_vl_2b_npu/RK35llm.h"

namespace qwen3_vl_2b_npu
{

class VlmServerNode : public rclcpp::Node
{
public:
    /// @brief 构造时通过 ROS 参数读取模型路径，加载双模型，
    ///        订阅摄像头话题并创建 /vlm_query 服务。
    /// @note  必须参数：  vlm_model, llm_model
    ///        可选参数：  camera_topic   (默认 /stereo/left_raw)
    ///                   max_new_tokens  (默认 2048)
    ///                   context_length  (默认 4096)
    explicit VlmServerNode();

    ~VlmServerNode() override = default;

private:
    // ─── 订阅回调：摄像头帧 → cv::Mat ───
    void imageCallback(const sensor_msgs::msg::Image::ConstSharedPtr& msg);

    // ─── 服务回调：接收问题 → 编码 + 推理 → 返回回复 ───
    void serviceCallback(
        const std::shared_ptr<qwen3_vl_2b_npu::srv::VlmQuery::Request> request,
        std::shared_ptr<qwen3_vl_2b_npu::srv::VlmQuery::Response> response);

    // ─── 核心成员 ───
    RK35llm     vlm_;                        // VLM 推理引擎
    cv::Mat     latest_frame_;               // 最新摄像头帧缓存
    rclcpp::Time last_frame_stamp_;          // 最新帧的采集时间
    std::mutex  frame_mtx_;                  // 保护 latest_frame_ + stamp

    // ─── ROS2 句柄 ───
    rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr img_sub_;
    rclcpp::Service<qwen3_vl_2b_npu::srv::VlmQuery>::SharedPtr vlm_srv_;
};

}  // namespace qwen3_vl_2b_npu

#endif  // VLMSERVERNODE_H_
