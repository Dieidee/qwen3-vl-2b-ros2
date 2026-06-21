#include "qwen3_vl_2b_npu/VlmServerNode.h"

#include <cv_bridge/cv_bridge.h>
#include <chrono>
#include <thread>
#include <algorithm>

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
    this->declare_parameter<double>("image_mode_timeout_s", 60.0);

    std::string vlm_model  = this->get_parameter("vlm_model").as_string();
    std::string llm_model  = this->get_parameter("llm_model").as_string();
    std::string cam_topic  = this->get_parameter("camera_topic").as_string();
    int max_tokens         = this->get_parameter("max_new_tokens").as_int();
    int ctx_len            = this->get_parameter("context_length").as_int();
    bool show_info         = this->get_parameter("show_info").as_bool();
    image_mode_timeout_s_  = this->get_parameter("image_mode_timeout_s").as_double();
    frame_timeout_s_       = this->get_parameter("frame_timeout_s").as_double();

    // ── 触发词 / 退出词 ─────────────────────────────────────
    trigger_words_ = {"[IMAGE]", "看图", "看一下", "看看", "启动图片",
                      "识别一下", "拍一下", "图片模式", "图像模式",
                      "描述一下", "这是什么", "里面有什么"};
    exit_words_    = {"退出看图", "不看图", "文字模式", "关闭图片",
                      "退出图片", "关闭图像", "图像退出"};

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

    // ── 4. 订阅 ASR ─────────────────────────────────────────
    asr_sub_ = this->create_subscription<std_msgs::msg::String>(
        "/asr/text", 10,
        std::bind(&VlmServerNode::asrCallback, this, std::placeholders::_1));
    RCLCPP_INFO(this->get_logger(), "Subscribed to /asr/text");

    // ── 5. 发布器 ───────────────────────────────────────────
    pub_result_ = this->create_publisher<std_msgs::msg::String>(
        "/vlm/result", 10);
    pub_stream_ = this->create_publisher<std_msgs::msg::String>(
        "/vlm/stream", 10);
    RCLCPP_INFO(this->get_logger(), "Publishers /vlm/result, /vlm/stream ready");

    // ── 6. 创建 Action 服务 ─────────────────────────────────
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
// ASR 回调
// ═══════════════════════════════════════════════════════════════════

void VlmServerNode::asrCallback(
    const std_msgs::msg::String::ConstSharedPtr& msg)
{
    std::string text = msg->data;

    // 去除首尾空白
    size_t start = text.find_first_not_of(" \t\n\r");
    size_t end   = text.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) return;   // 全空白
    text = text.substr(start, end - start + 1);

    if (text.empty()) return;

    // ── 1. 检查退出词 ─────────────────────────────────────
    std::string exit_word = match_exit_word(text);
    if (!exit_word.empty()) {
        is_image_mode_ = false;
        RCLCPP_INFO(this->get_logger(), "Image mode OFF (exit word: '%s')",
                    exit_word.c_str());
        auto msg_out = std_msgs::msg::String();
        msg_out.data = "已退出图片模式";
        pub_result_->publish(msg_out);
        return;
    }

    // ── 2. 检查触发词 ─────────────────────────────────────
    std::string trigger = match_trigger_word(text);
    bool needs_image = false;
    std::string question;

    if (!trigger.empty()) {
        is_image_mode_ = true;
        last_image_time_ = this->now();
        // 从文本中删除触发词
        size_t pos = text.find(trigger);
        question = text.substr(0, pos) + text.substr(pos + trigger.length());
        // 清理多余空格
        start = question.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) {
            question = "描述一下这张图片";
        } else {
            end = question.find_last_not_of(" \t\n\r");
            question = question.substr(start, end - start + 1);
        }
        needs_image = true;
        RCLCPP_INFO(this->get_logger(), "Image mode ON (trigger: '%s')",
                    trigger.c_str());
    } else {
        // ── 3. 检查图像模式超时 ────────────────────────────
        if (is_image_mode_) {
            double age = (this->now() - last_image_time_).seconds();
            if (age > image_mode_timeout_s_) {
                is_image_mode_ = false;
                RCLCPP_INFO(this->get_logger(),
                    "Image mode auto-off (timeout %.1fs)", age);
            }
        }
        needs_image = is_image_mode_;
        question = text;
    }

    // text 中只有触发词 → 自动补默认问题
    if (question.empty()) {
        question = "描述一下这张图片";
    }

    if (!needs_image && question == "clear") {
        // 纯文本模式下也支持 clear
        std::lock_guard<std::mutex> lk(npu_mutex_);
        vlm_.Ask("clear");
        auto msg_out = std_msgs::msg::String();
        msg_out.data = "KV cache cleared";
        pub_result_->publish(msg_out);
        return;
    }

    // ── 4. 启动推理线程 ───────────────────────────────────
    std::thread{
        [this, question, needs_image]() {
            process_query(question, needs_image, nullptr);
        }
    }.detach();
}

// ═══════════════════════════════════════════════════════════════════
// 触发词 / 退出词 匹配
// ═══════════════════════════════════════════════════════════════════

std::string VlmServerNode::match_trigger_word(const std::string& text)
{
    for (const auto& w : trigger_words_) {
        if (text.find(w) != std::string::npos) return w;
    }
    return "";
}

std::string VlmServerNode::match_exit_word(const std::string& text)
{
    for (const auto& w : exit_words_) {
        if (text.find(w) != std::string::npos) return w;
    }
    return "";
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
    std::thread{std::bind(&VlmServerNode::execute, this, goal_handle)}.detach();
}

void VlmServerNode::execute(std::shared_ptr<GoalHandle> goal_handle)
{
    process_query(goal_handle->get_goal()->question, true, goal_handle);
}

// ═══════════════════════════════════════════════════════════════════
// 核心推理
// ═══════════════════════════════════════════════════════════════════

void VlmServerNode::process_query(
    const std::string& question,
    bool needs_image,
    std::shared_ptr<GoalHandle> goal_handle)
{
    if (goal_handle) {
        goal_handle->execute();
    }

    auto t_start = std::chrono::steady_clock::now();
    auto result  = std::make_shared<StreamQuery::Result>();

    // 辅助 lambda：把 result 发布到 /vlm/result 并返回给 Action
    auto publish_and_finish = [&]() {
        auto result_msg = std_msgs::msg::String();
        result_msg.data = result->response;
        pub_result_->publish(result_msg);

        if (goal_handle) {
            if (result->success)
                goal_handle->succeed(result);
            else
                goal_handle->abort(result);
        }
    };

    // ── "clear" ────────────────────────────────────────────
    if (question == "clear") {
        std::lock_guard<std::mutex> lk(npu_mutex_);
        vlm_.Ask("clear");
        result->success  = true;
        result->response = "KV cache cleared";
        publish_and_finish();
        return;
    }

    // ── 图像模式：取帧 + 编码 ──────────────────────────────
    std::string processed_question = question;

    cv::Mat frame;
    if (needs_image) {
        rclcpp::Time stamp;
        {
            std::lock_guard<std::mutex> lock(frame_mtx_);
            if (latest_frame_.empty()) {
                result->success  = false;
                result->response = "No camera frame received yet";
                publish_and_finish();
                return;
            }
            frame = latest_frame_.clone();
            stamp = last_frame_stamp_;
        }

        double timeout_s = frame_timeout_s_;
        double age_s = (this->now() - stamp).seconds();
        if (age_s > timeout_s) {
            result->success  = false;
            result->response = "Frame too old: " + std::to_string(age_s) +
                               "s > " + std::to_string(timeout_s) + "s";
            RCLCPP_WARN(this->get_logger(),
                "Rejected stale frame (age=%.1fs)", age_s);
            publish_and_finish();
            return;
        }

        if (processed_question.find("<image>") == std::string::npos) {
            processed_question = "<image>\n" + processed_question;
        }
    }

    // ── NPU 推理（LoadImage + AskStream 在锁内） ──────────
    {
        std::lock_guard<std::mutex> lk(npu_mutex_);

        if (needs_image) {
            vlm_.LoadImage(frame);
        }

        auto stream_msg = std_msgs::msg::String();
        auto feedback   = std::make_shared<StreamQuery::Feedback>();

        std::string answer = vlm_.AskStream(processed_question,
            [&](const std::string& token) {
                stream_msg.data = token;
                pub_stream_->publish(stream_msg);

                if (goal_handle) {
                    feedback->token = token;
                    goal_handle->publish_feedback(feedback);
                }
            });

        result->success  = !answer.empty();
        result->response = answer.empty() ? "(empty response)" : answer;
    }

    // ── 完成 ──────────────────────────────────────────────
    publish_and_finish();

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_start).count();

    RCLCPP_INFO(this->get_logger(),
        "Inference done | %ld ms | image=%d | Q: '%.50s...' | A: '%.50s...'",
        elapsed, needs_image, question.c_str(), result->response.c_str());
}

}  // namespace qwen3_vl_2b_npu
