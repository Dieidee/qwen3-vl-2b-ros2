#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <qwen3_vl_2b_npu/action/stream_query.hpp>

using StreamQuery = qwen3_vl_2b_npu::action::StreamQuery;
using GoalHandle  = rclcpp_action::ClientGoalHandle<StreamQuery>;

int main(int argc, char ** argv)
{
    rclcpp::init(argc, argv);
    auto node = rclcpp::Node::make_shared("vlm_client");
    auto client = rclcpp_action::create_client<StreamQuery>(node, "/vlm_query");

    // ---------- 等待 Action 服务上线 ----------
    RCLCPP_INFO(node->get_logger(), "Waiting for /vlm_query ...");
    if (!client->wait_for_action_server(std::chrono::seconds(30))) {
        RCLCPP_ERROR(node->get_logger(),
                     "Action server not available after 30s");
        rclcpp::shutdown();
        return 1;
    }
    RCLCPP_INFO(node->get_logger(),
                "Connected. Type your question, or 'exit'/'clear'.");

    // ---------- REPL ----------
    std::string input;

    while (rclcpp::ok()) {
        std::cout << "\nUser: ";
        std::getline(std::cin, input);
        if (input.empty()) continue;
        if (input == "exit") break;

        // 构造 Goal
        auto goal_msg = StreamQuery::Goal();
        if (input.find("<image>") == std::string::npos) {
            goal_msg.question = input + " <image>";
        } else {
            goal_msg.question = input;
        }

        // feedback 回调：实时打印 token
        bool first_token = true;
        auto send_goal_opts = rclcpp_action::Client<StreamQuery>::SendGoalOptions();
        send_goal_opts.feedback_callback =
            [&first_token](
                GoalHandle::SharedPtr,
                const std::shared_ptr<const StreamQuery::Feedback> feedback) {
                if (first_token) {
                    std::cout << "Answer: ";
                    first_token = false;
                }
                std::cout << feedback->token << std::flush;
            };

        // 发送 goal
        auto future_goal_handle = client->async_send_goal(goal_msg, send_goal_opts);

        // 等待 goal 被接受
        auto status = rclcpp::spin_until_future_complete(node, future_goal_handle);
        if (status != rclcpp::FutureReturnCode::SUCCESS) {
            std::cerr << "Goal rejected or timeout" << std::endl;
            continue;
        }
        auto goal_handle = future_goal_handle.get();
        if (!goal_handle) {
            std::cerr << "Goal handle is null" << std::endl;
            continue;
        }

        // 等待结果
        auto future_result = client->async_get_result(goal_handle);
        status = rclcpp::spin_until_future_complete(node, future_result);
        if (status != rclcpp::FutureReturnCode::SUCCESS) {
            std::cerr << "Result timeout or interrupt" << std::endl;
            continue;
        }

        auto wrapped = future_result.get();
        if (wrapped.code != rclcpp_action::ResultCode::SUCCEEDED) {
            std::cerr << "\n[Server error] " << wrapped.result->response << std::endl;
        } else {
            std::cout << std::endl;  // token 流末尾换行
        }
    }

    rclcpp::shutdown();
    return 0;
}
