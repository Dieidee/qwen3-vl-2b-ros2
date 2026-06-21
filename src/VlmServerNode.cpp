#include "qwen3_vl_2b_npu/VlmServerNode.h"

#include <cv_bridge/cv_bridge.h>
#include <chrono>
#include <thread>

namespace qwen3_vl_2b_npu
{

// ═══════════════════════════════════════════════════════════════════
// 构造
// ═══════════════════════════════════════════════════════════════════

VlmServerNode::VlmServerNode()
    : Node("vlm_server")
{
    // ── 1. ROS 参数 ─────────────────────────────────────────
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
            "Missing required params: vlm_model / llm_model");
        rclcpp::shutdown();
        return;
    }

    // ── 2. 加载模型 ─────────────────────────────────────────
    vlm_.SetInfo(show_info);
    vlm_.SetSilence(true);

    RCLCPP_INFO(this->get_logger(), "Loading VLM: %s", vlm_model.c_str());
    RCLCPP_INFO(this->get_logger(), "Loading LLM: %s", llm_model.c_str());

    if (!vlm_.LoadModel(vlm_model, llm_model, max_tokens, ctx_len)) {
        RCLCPP_FATAL(this->get_logger(), "Model loading failed");
        rclcpp::shutdown();
        return;
    }
    RCLCPP_INFO(this->get_logger(), "Models loaded");

    // ── 3. 订阅摄像头 ───────────────────────────────────────
    img_sub_ = this->create_subscription<sensor_msgs::msg::Image>(
        cam_topic, 1,
        std::bind(&VlmServerNode::imageCallback, this, std::placeholders::_1));
    RCLCPP_INFO(this->get_logger(), "Subscribed to %s", cam_topic.c_str());

    // ── 4. 创建 Action 服务 ─────────────────────────────────
    action_srv_ = rclcpp_action::create_server<StreamQuery>(
        this,
        "/vlm_query",
        std::bind(&VlmServerNode::handle_goal,    this, std::placeholders::_1, std::placeholders::_2),
        std::bind(&VlmServerNode::handle_cancel,  this, std::placeholders::_1),
        std::bind(&VlmServerNode::handle_accepted, this, std::placeholders::_1));
    RCLCPP_INFO(this->get_logger(), "Action server /vlm_query ready");
}

// ═══════════════════════════════════════════════════════════════════
// 摄像头回调
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
// Action 回调
// ═══════════════════════════════════════════════════════════════════

rclcpp_action::GoalResponse VlmServerNode::handle_goal(
    const rclcpp_action::GoalUUID&,
    std::shared_ptr<const StreamQuery::Goal>)
{
    return rclcpp_action::GoalResponse::ACCEPT_AND_DEFER;
}

rclcpp_action::CancelResponse VlmServerNode::handle_cancel(
    std::shared_ptr<GoalHandle>)
{
    return rclcpp_action::CancelResponse::ACCEPT;
}

void VlmServerNode::handle_accepted(std::shared_ptr<GoalHandle> goal_handle)
{
    // 在独立线程执行推理，避免阻塞 executor
    std::thread{std::bind(&VlmServerNode::execute, this, goal_handle)}.detach();
}

// ═══════════════════════════════════════════════════════════════════
// 推理执行
// ═══════════════════════════════════════════════════════════════════

void VlmServerNode::execute(std::shared_ptr<GoalHandle> goal_handle)
{
    goal_handle->execute();  // ACCEPTED → EXECUTING（ACCEPT_AND_DEFER 要求手动）
    auto t_start = std::chrono::steady_clock::now();
    std::string question = goal_handle->get_goal()->question;

    auto result = std::make_shared<StreamQuery::Result>();

    // ── "clear" ──────────────────────────────────────────────
    if (question == "clear") {
        vlm_.Ask("clear");
        result->success  = true;
        result->response = "KV cache cleared";
        goal_handle->succeed(result);
        return;
    }

    // ── 取帧 ────────────────────────────────────────────────
    cv::Mat     frame;
    rclcpp::Time stamp;
    {
        std::lock_guard<std::mutex> lock(frame_mtx_);
        if (latest_frame_.empty()) {
            result->success  = false;
            result->response = "No camera frame received yet";
            goal_handle->abort(result);
            return;
        }
        frame = latest_frame_.clone();
        stamp = last_frame_stamp_;
    }

    // ── 帧过时检查 ──────────────────────────────────────────
    double timeout_s = this->get_parameter("frame_timeout_s").as_double();
    double age_s = (this->now() - stamp).seconds();
    if (age_s > timeout_s) {
        result->success  = false;
        result->response = "Frame too old: " + std::to_string(age_s) +
                           "s > " + std::to_string(timeout_s) + "s";
        RCLCPP_WARN(this->get_logger(),
            "Rejected stale frame (age=%.1fs)", age_s);
        goal_handle->abort(result);
        return;
    }

    // ── 编码 ────────────────────────────────────────────────
    vlm_.LoadImage(frame);

    // ── 流式推理 ───────────────────────────────────────────
    auto feedback = std::make_shared<StreamQuery::Feedback>();

    std::string answer = vlm_.AskStream(question,
        [&](const std::string& token) {
            feedback->token = token;
            goal_handle->publish_feedback(feedback);
        });

    result->success  = !answer.empty();
    result->response = answer.empty() ? "(empty response)" : answer;

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_start).count();

    RCLCPP_INFO(this->get_logger(),
        "Inference done | %ld ms | Q: '%.50s...' | A: '%.50s...'",
        elapsed, question.c_str(), answer.c_str());

    if (result->success)
        goal_handle->succeed(result);
    else
        goal_handle->abort(result);
}

}  // namespace qwen3_vl_2b_npu
