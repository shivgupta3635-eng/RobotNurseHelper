# Implementation Plan for AI Feature Integration

This document provides a concrete, step‑by‑step plan for adding each AI capability to the
RobotNurseHelper project.  The plan is organized by the milestones defined in **ROADMAP.md**.
All new code that interacts with NVIDIA NIM APIs will be written in **Python** (using the
provided `Python/` utilities or a new virtual environment) and called from the existing C++
application via a lightweight IPC mechanism (e.g., sockets or shared memory).  Existing C++
modules will remain unchanged unless a direct integration point is required.

---

## 1. Multilingual Speech Interaction

| Sub‑task | Description | Location / File | Owner | Status |
|----------|-------------|----------------|-------|--------|
| 1.1 Obtain API keys | Use the existing `json/shivpc.json` configuration. Add AI configuration entries only. Load API keys from environment variables. Never store API keys inside the repository. | `Server/json/shivpc.json` | – | ✅ (pending user) |
| 1.2 STT client (Python) | Wrapper around NVIDIA STT NIM endpoint. Expose a simple HTTP POST interface. | `Python/stt_client.py` | – | ☐ |
| 1.3 Audio capture bridge | Extend `ThreadPortAudio` to stream raw PCM to the Python STT process via a pipe. | `Server/ThreadPortAudio.cpp` | – | ☐ |
| 1.4 LLM request wrapper | Python module that forwards transcribed text to GLM‑5.2 / Nemotron‑Ultra and receives a response. | `Python/llm_client.py` | – | ☐ |
| 1.5 Optional translation | Use NVIDIA translation NIM if language switch is required. | `Python/translate.py` | – | ☐ |
| 1.6 TTS client (Python) | Wrapper for Chatterbox Multilingual TTS, returns audio bytes. | `Python/tts_client.py` | – | ☐ |
| 1.7 Audio playback bridge | Extend `ThreadProcessAudio` (or create a new thread) to play TTS audio via PortAudio. | `Server/ThreadProcessAudio.cpp` (new) | – | ☐ |
| 1.8 End‑to‑end test | Script that simulates a spoken query and verifies spoken answer. | `Python/e2e_speech_test.py` | – | ☐ |

### Integration notes
* The C++ side will launch the Python processes at startup (see `MainWindow::startThreads`).
* Communication can use ZeroMQ or simple TCP sockets – the existing `SendMessageManager` already
  handles protobuf messages, which we can reuse for audio data.

---

## 2. Real‑time Vision Pipeline (Extended)

The existing vision system (MediaPipeDetector) will be retained as the first‑stage perception module. A YOLOv11/v12 detector will be added after MediaPipe to provide object detection. The results from both modules will be fused in `ThreadProcessImage` to produce a unified perception output. A high‑level VisionClient (e.g., NVIDIA Nano Omni) will only be invoked on demand for scene‑level understanding.

| Sub‑task | Description | Location / File | Owner | Status |
|----------|-------------|----------------|-------|--------|
| 2.1 MediaPipe integration | Keep `MediaPipeDetector` as the first‑stage module. Add hand‑tracking initialization if not already present. | `Server/MediaPipeDetector.cpp` | – | ✅ |
| 2.2 YOLOv11/v12 wrapper | Create C++ class `YoloDetector` that loads the ONNX model and runs inference for objects: person, wheelchair, IV stand, medicine bottle, bed, chair, walking stick, medical equipment. | `Server/YoloDetector.hpp/.cpp` | – | ☐ |
| 2.3 Fusion in ThreadProcessImage | Extend `ThreadProcessImage` to combine MediaPipe landmarks with YOLO bounding boxes, improving accuracy and providing a unified perception result. | `Server/ThreadProcessImage.cpp` | – | ☐ |
| 2.4 Conditional VisionClient | Implement a lightweight client (`VisionClient`) that calls the Nano Omni (or another vision‑language model) **only** when high‑level scene understanding is required (user query, OCR, reasoning). | `Python/vision_client.py` (new) | – | ☐ |
| 2.5 Bridge to C++ for VisionClient | Add optional call from `ThreadProcessImage` to `VisionClient` when needed, and handle the returned description. | `Server/ThreadProcessImage.cpp` | – | ☐ |
| 2.6 Visualization | Update `VideoWindow` to overlay both MediaPipe landmarks and YOLO bounding boxes. Display Nano Omni captions when they are available. | `Server/VideoWindow.cpp` | – | ☐ |
| 2.7 End‑to‑end vision test | Verify the full pipeline: MediaPipe → YOLO → fusion → optional VisionClient → VideoWindow overlay. Use a sample video for testing. | `Python/e2e_vision_test.py` | – | ☐ |

---

## 3. OCR Capability

| Sub‑task | Description | Location / File | Owner | Status |
|----------|-------------|----------------|-------|--------|
| 3.1 OCR client (Python) | Wrapper for Nemotron OCR v2 NIM endpoint. | `Python/ocr_client.py` | – | ☐ |
| 3.2 Image capture | Re‑use the frame extraction from `ThreadProcessImage` for regions of interest (medicine bottles). | `Server/ThreadProcessImage.cpp` | – | ☐ |
| 3.3 OCR integration | After OCR result, send text to LLM for contextual response. | `Server/ThreadLLM.cpp` (modify) | – | ☐ |
| 3.4 Test OCR flow | Provide a set of sample label images and verify correct text extraction. | `Python/e2e_ocr_test.py` | – | ☐ |

---

## 4. Retrieval‑Augmented Generation (RAG)

| Sub‑task | Description | Location / File | Owner | Status |
|----------|-------------|----------------|-------|--------|
| 4.1 Document ingestion script | Python script that reads PDFs from `Document/` and indexes them with NeMo Retriever. | `Python/rag_ingest.py` | – | ☐ |
| 4.2 Retriever service | Long‑running Python process exposing a `search(query)` API over HTTP. | `Python/rag_service.py` | – | ☐ |
| 4.3 LLM‑RAG bridge | Modify `llm_client.py` to first query the Retriever; if a relevant passage is found, prepend it to the prompt. | `Python/llm_client.py` | – | ☐ |
| 4.4 C++ trigger | Add a new protobuf message type `RagQuery` and handling in `ThreadLLM` to forward queries to the Retriever service. | `Server/LLM/` (new proto) | – | ☐ |
| 4.5 End‑to‑end RAG test | Verify that a medical question returns a citation‑rich answer. | `Python/e2e_rag_test.py` | – | ☐ |

---

## 5. Safety & Content Filtering

| Sub‑task | Description | Location / File | Owner | Status |
|----------|-------------|----------------|-------|--------|
| 5.1 Safety client (Python) | Wrapper for Nemotron 3.5 Content Safety NIM endpoint. | `Python/safety_client.py` | – | ☐ |
| 5.2 Integration point | Before sending LLM response to TTS, pass it through the safety client. | `Python/llm_client.py` (modify) | – | ☐ |
| 5.3 Test unsafe content | Unit test that ensures flagged content is blocked or replaced. | `Python/e2e_safety_test.py` | – | ☐ |
## 6. Robot Integration

The goal is to embed all new AI capabilities directly into the existing RobotNurseHelper C++ server
using the current threading and protobuf‑based communication architecture.  No ROS 2, Nav2, or
additional ROS packages will be introduced.

| Sub‑task | Description | Location / File | Owner | Status |
|----------|-------------|----------------|-------|--------|
| 6.1 Integration design | Define how each AI module (MediaPipe, YOLO, OCR, STT, LLM, TTS, etc.) will be invoked from the C++ server and how results are passed back. | `IMPLEMENTATION_PLAN.md` (this section) | – | ☐ |
| 6.2 C++ ↔ Python bridge | Implement lightweight IPC (e.g., ZeroMQ or TCP sockets) or REST endpoints to call Python AI services from C++ threads. | `Server/ThreadProcessImage.cpp`, `Server/ThreadPortAudio.cpp`, etc. | – | ☐ |
| 6.3 Thread integration | Add or extend existing threads (e.g., `ThreadProcessImage`, `ThreadPortAudio`, `ThreadLLM`) to launch and communicate with the AI services. | Relevant thread source files | – | ☐ |
| 6.4 Message definitions | Extend protobuf messages if needed for audio, vision, OCR, or RAG data exchange. | `Server/Kebbi/RobotCommand.proto` (or new proto files) | – | ☐ |
| 6.5 Preserve existing functionality | Ensure current robot commands, UI, and state handling remain unchanged while new modules are added. | All server code | – | ☐ |

---

## 7. End‑to‑End Testing & Documentation

| Sub‑task | Description | Location / File | Owner | Status |
|----------|-------------|----------------|-------|--------|
| 7.1 CI pipeline | Add GitHub Actions workflow that builds the C++ project and runs all Python tests. | `.github/workflows/ci.yml` | – | ☐ |
| 7.2 Integration tests | Script that runs a full conversation (speech → vision → action) using mock audio/video inputs. | `Python/e2e_full_test.py` | – | ☐ |
| 7.3 Documentation update | Extend `README.md` with setup instructions for NVIDIA API keys, Python environment, and integration details (no ROS2). | `README.md` | – | ☐ |

---

## How to use this plan

1. **Create a feature branch** for each milestone (e.g., `feature/speech`).
2. Follow the table rows in order; mark the **Status** column as ✅ when completed.
3. Commit frequently and run the CI pipeline to ensure the existing robot functionality stays stable.
4. When a milestone is finished, merge it into `main` and proceed to the next one.

The plan is deliberately granular enough to be actionable while still allowing parallel work on
independent components (audio, vision, RAG).  Once you approve the plan, we can start
implementing the first milestone.
