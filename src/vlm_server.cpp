#include <rclcpp/rclcpp.hpp>
#include "qwen3_vl_2b_npu/VlmServerNode.h"

int main(int argc, char ** argv)
{
    rclcpp::init(argc, argv);

    // 模型路径、摄像头话题等由 VlmServerNode 通过 ROS 参数读取
    // 用法示例：
    //   ./vlm_server --ros-args
    //     -p vlm_model:=/path/to/vlm.rknn
    //     -p llm_model:=/path/to/llm.rkllm
    //     -p camera_topic:=/stereo/left_raw
    auto node = std::make_shared<qwen3_vl_2b_npu::VlmServerNode>();

    // 多线程执行器：订阅回调 + service 回调可并发，避免推理阻塞帧接收
    rclcpp::executors::MultiThreadedExecutor executor;
    executor.add_node(node);
    executor.spin();

    rclcpp::shutdown();
    return 0;
}
