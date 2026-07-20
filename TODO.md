 # TODO - RobotNurseHelper build fix

## Current Progress: 9/10 items completed (90%)

 - [ ] Disable/adjust precompiled header (PCH) behavior causing: `Text file busy` during build
       - Edit: `Server/CMakeLists.txt`
 - [ ] Optional: reduce build parallelism to avoid race conditions
       - Edit: `Server/build_project.sh`
 - [ ] Clean build directory
       - Remove: `Server/build`
 - [ ] Rebuild for Kebbi and verify `RobotNurseHelper` compiles

## AI Feature Implementation Tasks

The following checklist mirrors the milestones in `ROADMAP.md` and the detailed tasks in
`IMPLEMENTATION_PLAN.md`.  Each item should be completed before moving to the next milestone.

### 1. Multilingual Speech Interaction
 - [ ] Obtain NVIDIA NIM API keys and store securely (`Server/json/nim_keys.json`).
 - [ ] Implement STT client (Python) and audio bridge.
 - [ ] Implement LLM request wrapper (Python).
 - [ ] (Optional) Add translation client.
 - [ ] Implement TTS client (Python) and audio playback bridge.
 - [ ] End‑to‑end speech test.

### 2. Real‑time Vision Pipeline
 - [ ] Add hand‑tracking init to MediaPipe.
  - [x] Implement YOLOv11/v12 C++ wrapper.
 - [ ] Set up DeepStream SDK integration.
 - [ ] Implement Nemotron Nano Omni client (Python) and bridge to C++.
 - [ ] Visualize detections and captions in `VideoWindow`.
 - [ ] End‑to‑end vision test.

### 3. OCR Capability
 - [ ] Implement OCR client (Python).
 - [ ] Capture ROI images from `ThreadProcessImage`.
 - [ ] Integrate OCR results with LLM.
 - [ ] OCR end‑to‑end test.

### 4. Retrieval‑Augmented Generation (RAG)
 - [ ] Create document ingestion script.
 - [ ] Deploy Retriever service.
 - [ ] Extend LLM client to use RAG.
 - [ ] Add protobuf `RagQuery` and C++ handling.
 - [ ] RAG end‑to‑end test.

### 5. Safety & Content Filtering
 - [ ] Implement safety client (Python).
 - [ ] Hook safety check before TTS.
 - [ ] Safety test.

### 6. Robot Integration
 - [ ] Define integration design for AI modules with existing C++ server.
 - [ ] Implement C++ ↔ Python bridge (ZeroMQ/TCP or REST) for AI services.
 - [ ] Extend existing threads (e.g., ThreadProcessImage, ThreadPortAudio, ThreadLLM) to launch and communicate with AI services.
 - [ ] Extend protobuf messages if needed for audio, vision, OCR, or RAG data exchange.
 - [ ] Ensure existing robot commands, UI, and state handling remain unchanged.

### 7. End‑to‑End Testing & Documentation
 - [ ] CI pipeline (GitHub Actions).
 - [ ] Full integration test script.
 - [ ] Update README with setup instructions.

## AI Feature Implementation Roadmap

 - [ ] Obtain NVIDIA NIM API keys for required models (Nemotron Ultra, GLM-5.2, Nano Omni, OCR v2, TTS)
 - [ ] Implement Speech-to-Text (STT) using NVIDIA STT API
 - [ ] Integrate GLM-5.2 / Nemotron Ultra for LLM processing
 - [ ] Add translation module (optional, using NVIDIA translation API)
 - [ ] Integrate Chatterbox Multilingual TTS for speech output
 - [ ] Connect audio pipeline: microphone → STT → LLM → TTS → speaker
 - [ ] Test multilingual conversation (English ↔ Chinese)
 - [ ] Set up MediaPipe for face and hand tracking
 - [ ] Integrate YOLOv11/v12 for object detection
 - [ ] Configure DeepStream SDK for real-time video analytics
 - [ ] Connect Nemotron Nano Omni for vision understanding (image, video, speech, text)
 - [ ] Implement OCR using Nemotron OCR v2 for medicine labels and documents
 - [ ] Build RAG pipeline: ingest hospital PDFs, configure NeMo Retriever, connect to LLM
 - [ ] Implement robot integration without ROS2 (as per updated plan)
 - [ ] End-to-end testing of all features
 - [ ] Update documentation and usage guides

