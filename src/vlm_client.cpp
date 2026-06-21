#include <rclcpp/rclcpp.hpp>
#include <qwen3_vl_2b_npu/srv/vlm_query.hpp>

using VlmQuery = qwen3_vl_2b_npu::srv::VlmQuery;

int main(int argc, char ** argv)
{
    rclcpp::init(argc, argv);
    auto node = rclcpp::Node::make_shared("vlm_client");
    auto client = node->create_client<VlmQuery>("/vlm_query");

    // ---------- 等待服务上线 ----------
    RCLCPP_INFO(node->get_logger(), "Waiting for /vlm_query ...");
    if (!client->wait_for_service(std::chrono::seconds(30))) {
        RCLCPP_ERROR(node->get_logger(),
                     "Service not available after 30s. Is vlm_server running?");
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

        auto req = std::make_shared<VlmQuery::Request>();
        // 自动追加 <image> 标记，确保服务端走多模态推理路径
        if (input.find("<image>") == std::string::npos) {
            req->question = input + " <image>";
        } else {
            req->question = input;
        }

        auto future = client->async_send_request(req);

        // spin 直到 response 到达（或节点中断）
        auto status = rclcpp::spin_until_future_complete(node, future);

        if (status != rclcpp::FutureReturnCode::SUCCESS) {
            std::cerr << "Call failed (timeout / interrupt)" << std::endl;
            continue;
        }

        auto resp = future.get();
        if (resp->success) {
            std::cout << "Answer: " << resp->response << std::endl;
        } else {
            std::cerr << "[Server error] " << resp->response << std::endl;
        }
    }

    rclcpp::shutdown();
    return 0;
}
