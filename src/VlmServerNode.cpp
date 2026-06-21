#include "qwen3_vl_2b_npu/VlmServerNode.h"

#include <cv_bridge/cv_bridge.h>
#include <chrono>

namespace qwen3_vl_2b_npu
{

// ═══════════════════════════════════════════════════════════════════
// 构造 / 析构
// ═══════════════════════════════════════════════════════════════════

VlmServerNode::VlmServerNode()
    : Node("vlm_server")
{
    // ── 1. 声明 ROS 参数 ──────────────────────────────────────
    this->declare_parameter<std::string>("vlm_model");
    this->declare_parameter<std::string>("llm_model");
    this->declare_parameter<std::string>("camera_topic", "/stereo/left_raw");
    this->declare_parameter<int>("max_new_tokens", 2048);
    this->declare_parameter<int>("context_length", 4096);
    this->declare_parameter<bool>("show_info", false);
    this->declare_parameter<double>("frame_timeout_s", 5.0);

    std::string vlm_model  = this->get_parameter("vlm_model").as_string();
    std::string llm_model  = this->get_parameter("llm_model").as_string();
    std::string cam_topic  = this->get_parameter("camera_topic").as_string();
    int max_tokens         = this->get_parameter("max_new_tokens").as_int();
    int ctx_len            = this->get_parameter("context_length").as_int();
    bool show_info         = this->get_parameter("show_info").as_bool();

    if (vlm_model.empty() || llm_model.empty()) {
        RCLCPP_FATAL(this->get_logger(),
            "Missing required params: vlm_model / llm_model. "
            "Usage: --ros-args -p vlm_model:=/path/to/vlm.rknn -p llm_model:=/path/to/llm.rkllm");
        rclcpp::shutdown();
        return;
    }

    // ── 2. 加载 NPU 双模型 ────────────────────────────────────
    vlm_.SetInfo(show_info);
    vlm_.SetSilence(true);   // 服务模式：不在终端打印增量 token

    RCLCPP_INFO(this->get_logger(),
        "Loading VLM model: %s", vlm_model.c_str());
    RCLCPP_INFO(this->get_logger(),
        "Loading LLM model: %s", llm_model.c_str());

    if (!vlm_.LoadModel(vlm_model, llm_model, max_tokens, ctx_len)) {
        RCLCPP_FATAL(this->get_logger(), "Model loading failed");
        rclcpp::shutdown();
        return;
    }
    RCLCPP_INFO(this->get_logger(), "Models loaded successfully");

    // ── 3. 订阅摄像头话题 ─────────────────────────────────────
    img_sub_ = this->create_subscription<sensor_msgs::msg::Image>(
        cam_topic,
        1,   // queue_size=1, 只要最新帧
        std::bind(&VlmServerNode::imageCallback, this, std::placeholders::_1));
    RCLCPP_INFO(this->get_logger(), "Subscribed to %s", cam_topic.c_str());

    // ── 4. 创建 VLM 查询服务 ──────────────────────────────────
    vlm_srv_ = this->create_service<qwen3_vl_2b_npu::srv::VlmQuery>(
        "/vlm_query",
        std::bind(&VlmServerNode::serviceCallback, this,
                  std::placeholders::_1, std::placeholders::_2));
    RCLCPP_INFO(this->get_logger(), "Service /vlm_query ready");
}

// ═══════════════════════════════════════════════════════════════════
// 摄像头回调 — 只存最新帧
// ═══════════════════════════════════════════════════════════════════

void VlmServerNode::imageCallback(
    const sensor_msgs::msg::Image::ConstSharedPtr& msg)
{
    cv_bridge::CvImagePtr cv_ptr;
    try {
        cv_ptr = cv_bridge::toCvCopy(msg, "bgr8");
    } catch (cv_bridge::Exception& e) {
        RCLCPP_ERROR(this->get_logger(), "cv_bridge: %s", e.what());
        return;
    }

    std::lock_guard<std::mutex> lock(frame_mtx_);
    latest_frame_      = cv_ptr->image;
    last_frame_stamp_  = msg->header.stamp;
}

// ═══════════════════════════════════════════════════════════════════
// 服务回调 — 核心：取帧 → 编码 → 推理 → 回复
// ═══════════════════════════════════════════════════════════════════

void VlmServerNode::serviceCallback(
    const std::shared_ptr<qwen3_vl_2b_npu::srv::VlmQuery::Request> request,
    std::shared_ptr<qwen3_vl_2b_npu::srv::VlmQuery::Response> response)
{
    auto t_start = std::chrono::steady_clock::now();

    std::string question = request->question;

    // ── "clear" 命令：无需图像，直接清 KV Cache ──────────────
    if (question == "clear") {
        vlm_.Ask("clear");
        response->success  = true;
        response->response = "KV cache cleared";
        return;
    }

    cv::Mat     frame;
    rclcpp::Time stamp;
    {
        std::lock_guard<std::mutex> lock(frame_mtx_);
        if (latest_frame_.empty()) {
            response->success  = false;
            response->response = "No camera frame received yet";
            return;
        }
        frame = latest_frame_.clone();
        stamp = last_frame_stamp_;
    }

    // ── 帧过时检查 ──────────────────────────────────────────
    double timeout_s = this->get_parameter("frame_timeout_s").as_double();
    double age_s = (this->now() - stamp).seconds();
    if (age_s > timeout_s) {
        response->success  = false;
        response->response = "Frame too old: " + std::to_string(age_s) +
                             "s > " + std::to_string(timeout_s) + "s";
        RCLCPP_WARN(this->get_logger(),
            "Rejected stale frame (age=%.1fs, timeout=%.1fs)", age_s, timeout_s);
        return;
    }

    // ── 编码图像 ────────────────────────────────────────────
    vlm_.LoadImage(frame);

    // ── 推理 ────────────────────────────────────────────────
    std::string answer = vlm_.Ask(question);

    auto t_end   = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       t_end - t_start).count();

    response->success  = !answer.empty();
    response->response = answer.empty() ? "(empty response)" : answer;

    RCLCPP_INFO(this->get_logger(),
        "Inference done | %ld ms | question: '%.60s...' | answer: '%.60s...'",
        elapsed, question.c_str(), answer.c_str());
}

}  // namespace qwen3_vl_2b_npu
