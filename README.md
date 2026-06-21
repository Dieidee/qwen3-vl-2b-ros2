markdown
# Qwen3-VL-2B ROS2 NPU 推理服务节点

基于 Rockchip NPU 的视觉语言模型（VLM）推理服务节点。支持 ASR 语音输入触发、TTS 流式/完整输出、图像模式状态管理，为机器人提供多模态交互能力。

[![ROS2 Humble](https://img.shields.io/badge/ROS2-Humble-blue)](https://docs.ros.org/en/humble/)
[![Platform](https://img.shields.io/badge/Platform-RK3588-red)](https://www.rock-chips.com/)
[![License](https://img.shields.io/badge/License-Apache%202.0-green)](LICENSE)

---

## 📌 目录

- [特性](#-特性)
- [文件结构](#-文件结构)
- [依赖](#-依赖)
- [编译](#-编译)
- [运行](#-运行)
- [接口文档](#-接口文档给队友的对接说明)
- [流式输出原理](#-流式输出原理)
- [分支说明](#-分支说明)
- [常见问题](#-常见问题)

---

## ✨ 特性

- 🎯 **ASR 语音输入**：订阅 `/asr/text` 话题，自动触发推理
- 🖼️ **多模态视觉理解**：订阅 `/stereo/left_raw`，支持图像+文本联合推理
- 💬 **流式输出**：逐 Token 发布到 `/vlm/stream`，支持实时语音合成
- 🔄 **图像模式状态机**：触发词（"看一下"等）激活 60 秒图像模式，多轮对话无需重复说
- 🧪 **Action 调试接口**：保留 `/vlm_query` Action，方便手动测试
- 🚀 **NPU 加速**：集成 Rockchip RKNN / RKLLM 运行时，端侧高效推理

---

## 📂 文件结构
Qwen3-VL-2B-NPU-main/
├── action/ # Action 接口定义
│ └── StreamQuery.action # Goal: question → Feedback: token → Result: response
├── aarch64/ # 板端 NPU 依赖库（需自行放置）
│ ├── include/ # rknn_api.h, rkllm_api.h
│ └── library/ # librknnrt.so, librkllmrt.so
├── include/qwen3_vl_2b_npu/
│ ├── RK35llm.h # NPU 推理引擎封装（算法层）
│ └── VlmServerNode.h # ROS2 服务节点（应用层）
├── src/
│ ├── RK35llm.cpp # NPU 引擎实现
│ ├── VlmServerNode.cpp # 服务节点实现（含 ASR/TTS 对接）
│ ├── vlm_server.cpp # 服务端入口
│ └── vlm_client.cpp # Action 客户端（调试用）
├── models/ # 模型文件（需自行放置）
│ ├── qwen3-vl-2b-vision_rk3588.rknn
│ └── qwen3-vl-2b-instruct_w8a8_rk3588.rkllm
├── CMakeLists.txt
├── package.xml
└── README.md

text

---

## 🛠️ 依赖

- **ROS2 Humble**（或其他 LTS 版本）
- **OpenCV 4.x**
- **cv_bridge**
- **std_msgs**
- **Rockchip rknn / rkllm 运行时库**（版本需支持 `rkllm_set_chat_template` 等 API）

---

## 🚀 编译

### 1. 克隆仓库
```bash
git clone git@github.com:Dieidee/qwen3-vl-2b-ros2.git
cd qwen3-vl-2b-ros2
2. 放置 NPU 依赖库
将板端的 librknnrt.so、librkllmrt.so 及头文件放入 aarch64/ 对应目录：

bash
aarch64/
├── include/    # rknn_api.h, rkllm_api.h
└── library/    # librknnrt.so, librkllmrt.so
3. 放置模型文件
将 .rknn 和 .rkllm 模型放入 models/ 目录。

4. 编译
bash
colcon build --packages-select qwen3_vl_2b_npu \
  --cmake-args \
    -DRK_LIB_PATH=$(pwd)/aarch64/library \
    -DRK_INCLUDE_PATH=$(pwd)/aarch64/include
🏃‍♂️ 运行
环境准备（重要）
由于摄像头节点（final_depth）和 VLM 节点在不同的工作区，需按顺序加载环境：

终端1：启动摄像头节点（先编译 final_depth）
bash
cd ~/trae_ws
source /opt/ros/humble/setup.bash
source install/setup.bash
ros2 launch final_depth final_depth.launch.py
终端2：启动 VLM 服务端
bash
cd ~/TG_ws/Qwen3-VL-2B-NPU-main
source /opt/ros/humble/setup.bash
source ~/trae_ws/install/setup.bash   # 先加载摄像头工作区
source install/setup.bash              # 再加载 VLM 工作区
export LD_LIBRARY_PATH=$(pwd)/aarch64/library:$LD_LIBRARY_PATH

ros2 run qwen3_vl_2b_npu vlm_server \
  --ros-args \
    -p vlm_model:=$(pwd)/models/qwen3-vl-2b-vision_rk3588.rknn \
    -p llm_model:=$(pwd)/models/qwen3-vl-2b-instruct_w8a8_rk3588.rkllm
手动测试（模拟 ASR 输入）
终端3：测试触发词和推理
bash
# 模拟语音输入
ros2 topic pub /asr/text std_msgs/msg/String "data: '看一下桌子上的东西'"

# 查看 VLM 输出（给 TTS）
ros2 topic echo /vlm/result

# 查看流式输出（逐 Token）
ros2 topic echo /vlm/stream
Action 调试（手动触发，无需 ASR）
bash
# 使用 Action 客户端
ros2 action send_goal /vlm_query qwen3_vl_2b_npu/action/StreamQuery "{question: '描述一下这张图片'}"
📡 接口文档（给队友的对接说明）
1. 输入接口（ASR 节点）
项目	内容
话题	/asr/text
消息类型	std_msgs/msg/String
发布规则	只发最终识别结果（is_final=true），不发中间临时结果
特殊协议（可选）	在文本前加 [IMAGE] 可强制激活图像模式（如 "[IMAGE] 柜子有几层"）
2. 输出接口（TTS 节点）
项目	内容	用途
完整结果	/vlm/result（std_msgs/String）	推理完成后发布完整回答，适合非流式 TTS
流式输出	/vlm/stream（std_msgs/String）	逐 Token 发布，适合流式 TTS 或实时字幕
3. Action 调试接口
项目	内容
服务名	/vlm_query
类型	StreamQuery.action
Goal	string question
Feedback	string token（流式）
Result	string response（完整），bool success
4. 图像模式触发规则
节点内部维护一个 图像模式状态机：

触发方式	示例	说明
协议标记（最高优先级）	[IMAGE] 这个柜子有几层	ASR 端可主动插入，强制看图
关键词触发	看一下桌子上的东西	子串匹配（"看一下"、"看图"等）自动激活
状态保持	激活后 60 秒内所有问题自动带图	无需每句重复说"看一下"
退出方式	退出看图 / 文字模式	手动退出，并返回确认语
超时退出	60 秒无新请求自动退出	节省算力
触发词列表：

text
["[IMAGE]", "看图", "看一下", "看看", "启动图片", "识别一下", "拍一下", "图片模式", "图像模式", "描述一下", "这是什么", "里面有什么"]
退出词列表：

text
["退出看图", "不看图", "文字模式", "关闭图片", "退出图片", "关闭图像", "图像退出"]
🧠 流式输出原理
VLM 节点的流式输出基于 NPU 驱动的 Token 回调机制，实现“逐 Token 实时发布”。

三层链路
NPU 驱动层：rkllm_run 每生成一个 Token，立即调用 InstanceCallback

算法层：InstanceCallback 通过 token_callback_ 将 Token 传递给上层

应用层：process_query 中的 lambda 函数将 Token 发布到 /vlm/stream（同时发送 Action feedback）

关键代码路径
text
NPU 驱动 → InstanceCallback → token_callback_(result->text) → pub_stream_->publish(token)
Token 内容
/vlm/stream 发布的每个 Token 是模型的原始输出（包括标点符号、空格等），例如：

text
"今" → "天" → "的" → "天" → "气" → "。" → " " → "很" → "好"
📦 分支说明
分支	说明
main	稳定版本（流式 Action + C/S 基础功能）
feature/asr-tts-integration	ASR/TTS 对接 + 图像模式状态管理（当前开发分支）
feature/streaming-action	流式 Action 改造（已合并到 main）
⚠️ 常见问题
Q1: Package 'qwen3_vl_2b_npu' not found
原因：未加载工作区环境。
解决：

bash
source /opt/ros/humble/setup.bash
source ~/TG_ws/Qwen3-VL-2B-NPU-main/install/setup.bash
Q2: librknnrt.so: cannot open shared object file
原因：NPU 库路径未设置。
解决：

bash
export LD_LIBRARY_PATH=$(pwd)/aarch64/library:$LD_LIBRARY_PATH
Q3: 摄像头节点和 VLM 节点不在同一工作区
原因：final_depth 在 ~/trae_ws，VLM 在 ~/TG_ws/Qwen3-VL-2B-NPU-main。
解决：按顺序加载两个工作区：

bash
source ~/trae_ws/install/setup.bash   # 先加载摄像头
source ~/TG_ws/Qwen3-VL-2B-NPU-main/install/setup.bash   # 后加载 VLM（覆盖）
Q4: 触发了图像模式但返回 "No camera frame received yet"
原因：摄像头节点未启动或 /stereo/left_raw 无数据。
解决：

bash
# 先启动摄像头
cd ~/trae_ws
ros2 launch final_depth final_depth.launch.py

# 确认话题存在
ros2 topic list | grep stereo
Q5: TTS 收不到流式输出
原因：TTS 节点订阅了 /vlm/result 而非 /vlm/stream。
说明：

若 TTS 支持流式合成 → 订阅 /vlm/stream

若 TTS 只支持完整句子 → 订阅 /vlm/result

📄 License
Apache-2.0

🙏 致谢
Qwen-VL 模型

Rockchip NPU 工具链

ROS2 社区
