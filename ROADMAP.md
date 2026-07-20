# RobotNurseHelper AI Feature Roadmap

This roadmap outlines the high‑level milestones required to integrate the full NVIDIA AI stack into the RobotNurseHelper prototype.  Each milestone is independent and can be developed, tested, and merged incrementally.

## Milestones

1. **Multilingual Speech Interaction**
   - Speech‑to‑Text (STT) using NVIDIA NIM STT API
   - Large Language Model (GLM‑5.2 / Nemotron‑Ultra) for understanding
   - Optional translation layer (English ↔ Chinese)
   - Text‑to‑Speech (TTS) using Chatterbox Multilingual TTS
   - End‑to‑end audio pipeline (mic → STT → LLM → TTS → speaker)

2. **Real‑time Vision Pipeline**
   - MediaPipe for face/hand detection
   - YOLOv11/v12 for object detection (people, wheelchairs, IV stands, etc.)
   - DeepStream SDK for optimized video analytics
   - Nemotron‑Nano Omni for unified vision‑language understanding
   - Integration with existing `ThreadProcessImage` workflow

3. **OCR Capability**
   - NVIDIA Nemotron OCR v2 for reading medicine labels and hospital signage
   - Connect OCR output to the dialogue system for context‑aware responses

4. **Retrieval‑Augmented Generation (RAG)**
   - Ingest hospital PDFs, SOPs, medicine guides, etc.
   - Build a NeMo Retriever index (vector store)
   - Connect RAG pipeline to the LLM so queries are grounded in documents

5. **Safety & Content Filtering**
   - Nemotron 3.5 Content Safety model to filter unsafe responses before TTS
33 | 6. **Robot Integration**
34 |    - Integrate new AI modules with the existing C++ server.
35 |    - Use the existing threading and communication architecture.
36 |    - Connect MediaPipe, YOLO, OCR, Speech, Vision, and LLM modules directly to the current RobotNurseHelper server.
37 |    - Do not introduce ROS2, Nav2, or ROS packages.
38 |    - Preserve all existing functionality.

7. **End‑to‑End Testing & Documentation**
   - Automated tests for each pipeline component
   - User‑level integration tests (multilingual conversation, vision‑driven actions)
   - Update README and usage guides

Each milestone can be broken down into smaller tasks (see `IMPLEMENTATION_PLAN.md`).
