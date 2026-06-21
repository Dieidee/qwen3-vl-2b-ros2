# qwen3_vl_2b_npu

**Qwen3-VL-2B 视觉语言模型 ROS2 服务包**  
在 Rockchip RK3588 NPU（Rock 5、Orange Pi 5 等开发板）上运行 Qwen3-VL-2B 多模态大模型，通过 ROS2 Service 接口向其他节点提供实时视觉问答能力。

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)

---

## 目录

- [简介](#简介)
- [系统架构](#系统架构)
- [性能参考](#性能参考)
- [依赖项](#依赖项)
- [安装](#安装)
- [模型下载](#模型下载)
- [构建](#构建)
- [使用方法](#使用方法)
- [ROS2 接口](#ros2-接口)
- [节点参数](#节点参数)
- [项目结构](#项目结构)

---

## 简介

本包将 [Q-engineering](https://github.com/Qengineering/Qwen3-VL-2B-NPU) 的 Qwen3-VL-2B NPU 推理库封装为标准 ROS2 节点，实现了：

- **摄像头订阅**：自动获取最新图像帧（默认话题 `/stereo/left_raw`）
- **ROS2 Service**：暴露 `/vlm_query` 服务，接收文本问题，返回模型回答
- **多线程执行**：图像订阅与推理服务并发执行，避免互相阻塞
- **帧时效检查**：超过设定时间的陈旧帧会被拒绝，确保推理基于最新画面
- **对话历史**：模型保留上下文，支持多轮对话（发送 `clear` 可重置）

模型量化规格：LLM 使用 **w8a8**，VLM 视觉编码器使用 **fp16**，分辨率 448×448。

---

## 系统架构

```
摄像头节点
    │  sensor_msgs/Image
    ▼
┌─────────────────────────────┐
│       vlm_server 节点        │
│  ┌──────────────────────┐   │
│  │   VlmServerNode      │   │
│  │  ┌────────────────┐  │   │
│  │  │  RK35llm       │  │   │
│  │  │  (RKNN视觉编码) │  │   │
│  │  │  (RKLLM语言模型)│  │   │
│  │  └────────────────┘  │   │
│  └──────────────────────┘   │
└───────────┬─────────────────┘
            │ /vlm_query (VlmQuery.srv)
            ▼
       vlm_client 节点
       （终端 REPL 交互）
```

---

## 性能参考

| 指标 | 数值 |
|------|------|
| 内存占用 | ~3.1 GB |
| LLM 冷启动 | ~21.9 s |
| LLM 热启动 | ~2.6 s |
| VLM 冷启动 | ~10.0 s |
| VLM 热启动 | ~0.9 s |
| 推理速度 | ~11.5 tokens/s |
| 输入分辨率 | 448 × 448 |

> 测试平台：Rock 5C（RK3588），模型量化：w8a8（LLM）+ fp16（VLM）

---

## 依赖项

### 硬件
- Rockchip RK3588 SoC 开发板（Rock 5、Orange Pi 5 等）

### 软件
| 依赖 | 版本要求 |
|------|----------|
| ROS2 | Humble 或更高 |
| OpenCV | 4.x（64-bit） |
| rkllm-runtime | ≥ 1.2.3 |
| rknpu driver | ≥ 0.9.8 |
| C++ | 17 |

### 安装系统依赖

```bash
sudo apt-get update && sudo apt-get upgrade
sudo apt-get install cmake wget curl libopencv-dev
```

---

## 安装

### 1. 克隆本仓库到 ROS2 工作空间

```bash
cd ~/ros2_ws/src
git clone <本仓库地址> qwen3_vl_2b_npu
```

### 2. 安装 RKLLM / RKNN 运行时库

仓库已附带所需版本的库文件与头文件：

```bash
cd qwen3_vl_2b_npu/aarch64/library
sudo cp ./*.so /usr/local/lib

cd ../include
sudo cp ./*.h /usr/local/include

sudo ldconfig
```

---

## 模型下载

需要以下两个模型文件（共约 2.3 GB），下载后放入包内的 `models/` 目录：

| 文件 | 说明 |
|------|------|
| `qwen3-vl-2b-vision_rk3588.rknn` | 视觉编码器（RKNN 格式） |
| `qwen3-vl-2b-instruct_w8a8_rk3588.rkllm` | 语言模型（RKLLM 格式，w8a8 量化） |

**下载地址（Sync.com）：**
- [qwen3-vl-2b-instruct_w8a8_rk3588.rkllm](https://ln5.sync.com/dl/6cd2e45d0#swbgmrgn-xqjwb4pn-h3fizzg5-vb3jvfxd)
- [qwen3-vl-2b-vision_rk3588.rknn](https://ln5.sync.com/dl/d1a22a380#kshmvhzf-ma8xhheb-mbx2x47f-qp5fajj5)

也可从 Rockchip 官方模型库下载（完整包约 44 GB）：[rkllm_model_zoo](https://console.box.lenovo.com/l/l0tXb8)（fetch code: `rkllm`）

---

## 构建

```bash
cd ~/ros2_ws
colcon build --packages-select qwen3_vl_2b_npu
source install/setup.bash
```

如果 RKLLM/RKNN 库不在 `/usr/local/lib`，可通过 CMake 参数指定路径：

```bash
colcon build --packages-select qwen3_vl_2b_npu \
    --cmake-args -DRK_LIB_PATH=/path/to/libs -DRK_INCLUDE_PATH=/path/to/includes
```

---

## 使用方法

### 启动 vlm_server（服务端节点）

```bash
ros2 run qwen3_vl_2b_npu vlm_server --ros-args \
    -p vlm_model:=/path/to/models/qwen3-vl-2b-vision_rk3588.rknn \
    -p llm_model:=/path/to/models/qwen3-vl-2b-instruct_w8a8_rk3588.rkllm \
    -p camera_topic:=/stereo/left_raw
```

### 启动 vlm_client（交互式客户端）

```bash
ros2 run qwen3_vl_2b_npu vlm_client
```

客户端启动后进入 REPL 交互模式：

```
User: <image>这张图片里有什么？
Answer: 图片中显示的是...

User: 详细描述一下背景
Answer: 背景中包含...

User: clear          # 清除对话历史（重置 KV Cache）
User: exit           # 退出客户端
```

> **注意**：客户端会自动在问题中追加 `<image>` 标记（若不存在），确保服务端走多模态推理路径。

---

## ROS2 接口

### 服务：`/vlm_query`

服务类型定义（`srv/VlmQuery.srv`）：

```
# Request
string question
---
# Response
string response
bool   success
```

**特殊指令**（通过 `question` 字段发送）：

| 指令 | 说明 |
|------|------|
| `clear` | 清除对话历史，重置 KV Cache |
| `<image>` | 在问题中包含此标记，表示需要引用当前摄像头图像 |

### 订阅的话题

| 话题 | 消息类型 | 说明 |
|------|----------|------|
| `/stereo/left_raw`（可配置） | `sensor_msgs/Image` | 摄像头输入图像（只保留最新帧） |

---

## 节点参数

`vlm_server` 节点支持以下 ROS2 参数：

| 参数名 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `vlm_model` | string | **必填** | RKNN 视觉编码器模型路径 |
| `llm_model` | string | **必填** | RKLLM 语言模型路径 |
| `camera_topic` | string | `/stereo/left_raw` | 订阅的摄像头话题名 |
| `max_new_tokens` | int | `2048` | 单次推理最大生成 token 数 |
| `context_length` | int | `4096` | 模型上下文窗口长度（含输入+输出） |
| `show_info` | bool | `false` | 是否打印模型加载详细信息 |
| `frame_timeout_s` | double | `5.0` | 摄像头帧最大允许时效（秒），超时拒绝推理 |

---

## 项目结构

```
qwen3_vl_2b_npu/
├── aarch64/
│   └── include/            # Rockchip 运行时头文件
│       ├── rkllm.h
│       ├── rknn_api.h
│       ├── rknn_custom_op.h
│       └── rknn_matmul_api.h
├── include/
│   └── qwen3_vl_2b_npu/
│       ├── RK35llm.h       # NPU 推理引擎封装类
│       └── VlmServerNode.h # ROS2 服务节点类
├── models/
│   └── README.md           # 模型下载说明
├── src/
│   ├── RK35llm.cpp         # RKNN + RKLLM 推理引擎实现
│   ├── VlmServerNode.cpp   # ROS2 节点：订阅摄像头 + 提供服务
│   ├── vlm_server.cpp      # 服务端 main()
│   └── vlm_client.cpp      # 客户端 REPL main()
├── srv/
│   └── VlmQuery.srv        # 自定义 ROS2 服务消息定义
├── CMakeLists.txt
├── package.xml
└── README.md
```

---

## 相关资源

- [Qwen3 技术报告](https://arxiv.org/pdf/2505.09388)
- [Qwen3-VL 原始 NPU 项目（Q-engineering）](https://github.com/Qengineering/Qwen3-VL-2B-NPU)
- [Qwen3-VL-4B HuggingFace](https://huggingface.co/Qwen/Qwen3-VL-4B-Instruct)
- [在线演示（Rock5GPT）](https://rock5gpt.qengineering.eu)
- [OpenCV 安装指南](https://qengineering.eu/install-opencv-on-raspberry-64-os.html)
