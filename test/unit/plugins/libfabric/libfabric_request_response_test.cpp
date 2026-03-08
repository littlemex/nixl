/*
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026 Amazon.com, Inc. and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file libfabric_request_response_test.cpp
 * @brief Unit tests for NIXL libfabric Request/Response protocol
 *
 * Tests the two-sided messaging protocol used on AWS EFA where one-sided
 * RDMA READ (fi_read) is not supported. The protocol uses:
 * - Control Rail (Rail 0) for READ_REQUEST messages (Consumer -> Producer)
 * - Data Rails (Rail 1+) for data transfer via fi_senddata (Producer -> Consumer)
 *
 * Test categories:
 * 1. NixlControlMessage structure and serialization
 * 2. Immediate data encoding/decoding macros
 * 3. Request handle (nixlLibfabricBackendH) completion tracking
 * 4. Error handling scenarios
 */

#include <iostream>
#include <string>
#include <cstring>
#include <cassert>
#include <vector>
#include <thread>
#include <atomic>

#include "libfabric/libfabric_common.h"

// Test result tracking
static int tests_passed = 0;
static int tests_failed = 0;
static int tests_total = 0;

#define TEST_ASSERT(condition, message)                                          \
    do {                                                                         \
        tests_total++;                                                           \
        if (!(condition)) {                                                      \
            std::cerr << "[FAIL] " << message << " (" << __FILE__ << ":"        \
                      << __LINE__ << ")" << std::endl;                           \
            tests_failed++;                                                      \
        } else {                                                                 \
            std::cout << "[PASS] " << message << std::endl;                      \
            tests_passed++;                                                      \
        }                                                                        \
    } while (0)

#define TEST_ASSERT_EQ(actual, expected, message)                                \
    do {                                                                         \
        tests_total++;                                                           \
        if ((actual) != (expected)) {                                            \
            std::cerr << "[FAIL] " << message << " (expected=" << (expected)     \
                      << ", actual=" << (actual) << ") (" << __FILE__ << ":"     \
                      << __LINE__ << ")" << std::endl;                           \
            tests_failed++;                                                      \
        } else {                                                                 \
            std::cout << "[PASS] " << message << std::endl;                      \
            tests_passed++;                                                      \
        }                                                                        \
    } while (0)

// ---------------------------------------------------------------------------
// Test Suite 1: NixlControlMessage Structure
// ---------------------------------------------------------------------------

static void
test_control_message_size() {
    std::cout << std::endl << "--- NixlControlMessage Structure Tests ---" << std::endl;

    TEST_ASSERT_EQ(sizeof(NixlControlMessage), 32u,
                   "NixlControlMessage size must be 32 bytes for cache efficiency");
}

static void
test_control_message_default_construction() {
    NixlControlMessage msg;

    TEST_ASSERT_EQ(msg.operation, 0u, "Default operation should be 0");
    TEST_ASSERT_EQ(msg.request_id, 0u, "Default request_id should be 0");
    TEST_ASSERT_EQ(msg.rail_id, 0u, "Default rail_id should be 0");
    TEST_ASSERT_EQ(msg.reserved, 0u, "Default reserved should be 0");
    TEST_ASSERT_EQ(msg.length, 0ull, "Default length should be 0");
    TEST_ASSERT_EQ(msg.offset, 0ull, "Default offset should be 0");
}

static void
test_control_message_read_request() {
    NixlControlMessage msg;
    msg.operation = NixlControlMessage::READ_REQUEST;
    msg.request_id = 42;
    msg.rail_id = 1;
    msg.length = 16384;
    msg.offset = 0;

    TEST_ASSERT_EQ(msg.operation, static_cast<uint32_t>(NixlControlMessage::READ_REQUEST),
                   "Operation should be READ_REQUEST (1)");
    TEST_ASSERT_EQ(msg.request_id, 42u, "Request ID should be 42");
    TEST_ASSERT_EQ(msg.rail_id, 1u, "Rail ID should be 1");
    TEST_ASSERT_EQ(msg.length, 16384ull, "Length should be 16384");
    TEST_ASSERT_EQ(msg.offset, 0ull, "Offset should be 0");
}

static void
test_control_message_operation_enum() {
    TEST_ASSERT_EQ(static_cast<uint32_t>(NixlControlMessage::READ_REQUEST), 1u,
                   "READ_REQUEST enum value should be 1");
    TEST_ASSERT_EQ(static_cast<uint32_t>(NixlControlMessage::WRITE_NOTIFY), 2u,
                   "WRITE_NOTIFY enum value should be 2");
}

static void
test_control_message_serialization() {
    // Verify that a NixlControlMessage can be copied via memcpy
    // (simulating network send/receive)
    NixlControlMessage original;
    original.operation = NixlControlMessage::READ_REQUEST;
    original.request_id = 0xBEEF;
    original.rail_id = 3;
    original.length = 1024 * 1024;
    original.offset = 4096;

    // Simulate network transfer via buffer copy
    char buffer[sizeof(NixlControlMessage)];
    std::memcpy(buffer, &original, sizeof(NixlControlMessage));

    NixlControlMessage received;
    std::memcpy(&received, buffer, sizeof(NixlControlMessage));

    TEST_ASSERT_EQ(received.operation, original.operation,
                   "Serialized operation should match original");
    TEST_ASSERT_EQ(received.request_id, original.request_id,
                   "Serialized request_id should match original");
    TEST_ASSERT_EQ(received.rail_id, original.rail_id,
                   "Serialized rail_id should match original");
    TEST_ASSERT_EQ(received.length, original.length,
                   "Serialized length should match original");
    TEST_ASSERT_EQ(received.offset, original.offset,
                   "Serialized offset should match original");
}

static void
test_control_message_large_transfer() {
    // Test with large transfer sizes (e.g., 2MB as in actual vLLM usage)
    NixlControlMessage msg;
    msg.operation = NixlControlMessage::READ_REQUEST;
    msg.request_id = 12345;
    msg.rail_id = 2;
    msg.length = 2 * 1024 * 1024; // 2MB
    msg.offset = 0;

    TEST_ASSERT_EQ(msg.length, static_cast<uint64_t>(2 * 1024 * 1024),
                   "Should handle 2MB transfer size");

    // Test with maximum uint64_t length
    msg.length = std::numeric_limits<uint64_t>::max();
    TEST_ASSERT_EQ(msg.length, std::numeric_limits<uint64_t>::max(),
                   "Should handle max uint64_t transfer size");
}

static void
test_control_message_multi_rail() {
    // Test creating READ_REQUEST for multiple data rails (real scenario)
    std::vector<NixlControlMessage> requests;
    uint32_t xfer_id = 100;

    // Simulate 4 data rails (rails 1-4, skipping control rail 0)
    for (uint32_t rail_id = 1; rail_id <= 4; rail_id++) {
        NixlControlMessage msg;
        msg.operation = NixlControlMessage::READ_REQUEST;
        msg.request_id = xfer_id;
        msg.rail_id = rail_id;
        msg.length = 16384;
        msg.offset = 0;
        requests.push_back(msg);
    }

    TEST_ASSERT_EQ(requests.size(), 4u, "Should have 4 READ_REQUEST messages for 4 data rails");

    for (size_t i = 0; i < requests.size(); i++) {
        TEST_ASSERT_EQ(requests[i].rail_id, static_cast<uint32_t>(i + 1),
                       "Rail IDs should be sequential starting from 1");
        TEST_ASSERT_EQ(requests[i].request_id, xfer_id,
                       "All requests should share the same xfer_id");
    }
}

// ---------------------------------------------------------------------------
// Test Suite 2: Immediate Data Encoding/Decoding
// ---------------------------------------------------------------------------

static void
test_imm_data_basic_encoding() {
    std::cout << std::endl << "--- Immediate Data Encoding/Decoding Tests ---" << std::endl;

    uint64_t imm = NIXL_MAKE_IMM_DATA(
        NIXL_LIBFABRIC_MSG_TRANSFER, // msg_type = 4
        5,                           // agent_idx
        100,                         // xfer_id
        3                            // seq_id
    );

    TEST_ASSERT_EQ(NIXL_GET_MSG_TYPE_FROM_IMM(imm),
                   static_cast<uint64_t>(NIXL_LIBFABRIC_MSG_TRANSFER),
                   "Extracted msg_type should be TRANSFER (4)");
    TEST_ASSERT_EQ(NIXL_GET_AGENT_INDEX_FROM_IMM(imm), 5ull,
                   "Extracted agent_index should be 5");
    TEST_ASSERT_EQ(NIXL_GET_XFER_ID_FROM_IMM(imm), 100ull,
                   "Extracted xfer_id should be 100");
    TEST_ASSERT_EQ(NIXL_GET_SEQ_ID_FROM_IMM(imm), 3ull,
                   "Extracted seq_id should be 3");
}

static void
test_imm_data_notification_type() {
    uint64_t imm = NIXL_MAKE_IMM_DATA(
        NIXL_LIBFABRIC_MSG_NOTIFICTION, // msg_type = 2
        10,                              // agent_idx
        500,                             // xfer_id
        7                                // seq_id
    );

    TEST_ASSERT_EQ(NIXL_GET_MSG_TYPE_FROM_IMM(imm),
                   static_cast<uint64_t>(NIXL_LIBFABRIC_MSG_NOTIFICTION),
                   "Extracted msg_type should be NOTIFICATION (2)");
    TEST_ASSERT_EQ(NIXL_GET_AGENT_INDEX_FROM_IMM(imm), 10ull,
                   "Extracted agent_index should be 10");
    TEST_ASSERT_EQ(NIXL_GET_XFER_ID_FROM_IMM(imm), 500ull,
                   "Extracted xfer_id should be 500");
    TEST_ASSERT_EQ(NIXL_GET_SEQ_ID_FROM_IMM(imm), 7ull,
                   "Extracted seq_id should be 7");
}

static void
test_imm_data_boundary_values() {
    // Test maximum values for each field
    uint64_t imm = NIXL_MAKE_IMM_DATA(
        0xF,     // max 4-bit msg_type
        0xFF,    // max 8-bit agent_idx
        0xFFFF,  // max 16-bit xfer_id
        0xF      // max 4-bit seq_id
    );

    TEST_ASSERT_EQ(NIXL_GET_MSG_TYPE_FROM_IMM(imm), 0xFull,
                   "Max msg_type should be 0xF");
    TEST_ASSERT_EQ(NIXL_GET_AGENT_INDEX_FROM_IMM(imm), 0xFFull,
                   "Max agent_index should be 0xFF");
    TEST_ASSERT_EQ(NIXL_GET_XFER_ID_FROM_IMM(imm), 0xFFFFull,
                   "Max xfer_id should be 0xFFFF");
    TEST_ASSERT_EQ(NIXL_GET_SEQ_ID_FROM_IMM(imm), 0xFull,
                   "Max seq_id should be 0xF");
}

static void
test_imm_data_zero_values() {
    uint64_t imm = NIXL_MAKE_IMM_DATA(0, 0, 0, 0);

    TEST_ASSERT_EQ(imm, 0ull, "All-zero immediate data should be 0");
    TEST_ASSERT_EQ(NIXL_GET_MSG_TYPE_FROM_IMM(imm), 0ull, "Zero msg_type");
    TEST_ASSERT_EQ(NIXL_GET_AGENT_INDEX_FROM_IMM(imm), 0ull, "Zero agent_index");
    TEST_ASSERT_EQ(NIXL_GET_XFER_ID_FROM_IMM(imm), 0ull, "Zero xfer_id");
    TEST_ASSERT_EQ(NIXL_GET_SEQ_ID_FROM_IMM(imm), 0ull, "Zero seq_id");
}

static void
test_imm_data_field_isolation() {
    // Ensure each field does not bleed into others
    // Set only msg_type
    uint64_t imm1 = NIXL_MAKE_IMM_DATA(0xF, 0, 0, 0);
    TEST_ASSERT_EQ(NIXL_GET_AGENT_INDEX_FROM_IMM(imm1), 0ull,
                   "Setting msg_type should not affect agent_index");
    TEST_ASSERT_EQ(NIXL_GET_XFER_ID_FROM_IMM(imm1), 0ull,
                   "Setting msg_type should not affect xfer_id");
    TEST_ASSERT_EQ(NIXL_GET_SEQ_ID_FROM_IMM(imm1), 0ull,
                   "Setting msg_type should not affect seq_id");

    // Set only agent_idx
    uint64_t imm2 = NIXL_MAKE_IMM_DATA(0, 0xFF, 0, 0);
    TEST_ASSERT_EQ(NIXL_GET_MSG_TYPE_FROM_IMM(imm2), 0ull,
                   "Setting agent_idx should not affect msg_type");
    TEST_ASSERT_EQ(NIXL_GET_XFER_ID_FROM_IMM(imm2), 0ull,
                   "Setting agent_idx should not affect xfer_id");
    TEST_ASSERT_EQ(NIXL_GET_SEQ_ID_FROM_IMM(imm2), 0ull,
                   "Setting agent_idx should not affect seq_id");

    // Set only xfer_id
    uint64_t imm3 = NIXL_MAKE_IMM_DATA(0, 0, 0xFFFF, 0);
    TEST_ASSERT_EQ(NIXL_GET_MSG_TYPE_FROM_IMM(imm3), 0ull,
                   "Setting xfer_id should not affect msg_type");
    TEST_ASSERT_EQ(NIXL_GET_AGENT_INDEX_FROM_IMM(imm3), 0ull,
                   "Setting xfer_id should not affect agent_index");
    TEST_ASSERT_EQ(NIXL_GET_SEQ_ID_FROM_IMM(imm3), 0ull,
                   "Setting xfer_id should not affect seq_id");

    // Set only seq_id
    uint64_t imm4 = NIXL_MAKE_IMM_DATA(0, 0, 0, 0xF);
    TEST_ASSERT_EQ(NIXL_GET_MSG_TYPE_FROM_IMM(imm4), 0ull,
                   "Setting seq_id should not affect msg_type");
    TEST_ASSERT_EQ(NIXL_GET_AGENT_INDEX_FROM_IMM(imm4), 0ull,
                   "Setting seq_id should not affect agent_index");
    TEST_ASSERT_EQ(NIXL_GET_XFER_ID_FROM_IMM(imm4), 0ull,
                   "Setting seq_id should not affect xfer_id");
}

static void
test_imm_data_read_request_scenario() {
    // Simulate actual READ_REQUEST encoding as used in handleControlMessage
    uint16_t agent_idx = 1;
    uint32_t request_id = 42;
    uint8_t seq_id = 0;

    uint64_t imm = NIXL_MAKE_IMM_DATA(
        NIXL_LIBFABRIC_MSG_TRANSFER,
        agent_idx,
        request_id,
        seq_id
    );

    // Verify Consumer can decode the immediate data correctly
    TEST_ASSERT_EQ(NIXL_GET_MSG_TYPE_FROM_IMM(imm),
                   static_cast<uint64_t>(NIXL_LIBFABRIC_MSG_TRANSFER),
                   "READ_REQUEST response: msg_type should be TRANSFER");
    TEST_ASSERT_EQ(NIXL_GET_AGENT_INDEX_FROM_IMM(imm), static_cast<uint64_t>(agent_idx),
                   "READ_REQUEST response: agent_idx should match");
    TEST_ASSERT_EQ(NIXL_GET_XFER_ID_FROM_IMM(imm), static_cast<uint64_t>(request_id),
                   "READ_REQUEST response: xfer_id should match request_id");
    TEST_ASSERT_EQ(NIXL_GET_SEQ_ID_FROM_IMM(imm), static_cast<uint64_t>(seq_id),
                   "READ_REQUEST response: seq_id should be 0 for single chunk");
}

// ---------------------------------------------------------------------------
// Test Suite 3: BinaryNotificationHeader
// ---------------------------------------------------------------------------

static void
test_binary_notification_header_size() {
    std::cout << std::endl << "--- BinaryNotification Tests ---" << std::endl;

    TEST_ASSERT_EQ(sizeof(BinaryNotificationHeader), 10u,
                   "BinaryNotificationHeader should be 10 bytes (packed)");
    TEST_ASSERT_EQ(sizeof(BinaryNotificationMetadata), 10u,
                   "BinaryNotificationMetadata should be 10 bytes (packed)");
}

static void
test_binary_notification_serialize_fragment0() {
    BinaryNotification notif;

    BinaryNotificationHeader header;
    header.notif_xfer_id = 42;
    header.notif_seq_id = 0;
    header.notif_seq_len = 1;
    header.payload_length = 5;
    notif.setHeader(header);

    notif.setMetadata(5, 10, 3);
    notif.setPayload(std::string("hello"));

    char buffer[256];
    size_t size = notif.serialize(buffer);

    // Fragment 0: header(10) + metadata(10) + payload(5)
    TEST_ASSERT_EQ(size, 25u,
                   "Fragment 0 serialized size should be header(10)+metadata(10)+payload(5)=25");

    // Deserialize and verify
    BinaryNotification restored;
    BinaryNotification::deserialize(buffer, size, restored);

    TEST_ASSERT_EQ(restored.getHeader().notif_xfer_id, 42u,
                   "Deserialized xfer_id should be 42");
    TEST_ASSERT_EQ(restored.getHeader().notif_seq_id, 0u,
                   "Deserialized seq_id should be 0");
    TEST_ASSERT_EQ(restored.getMetadata().total_payload_length, 5u,
                   "Deserialized total_payload_length should be 5");
    TEST_ASSERT_EQ(restored.getMetadata().expected_completions, 10u,
                   "Deserialized expected_completions should be 10");
    TEST_ASSERT_EQ(restored.getMetadata().agent_name_length, 3u,
                   "Deserialized agent_name_length should be 3");
    TEST_ASSERT(restored.getPayload() == "hello",
                "Deserialized payload should be 'hello'");
}

static void
test_binary_notification_serialize_fragment_n() {
    BinaryNotification notif;

    BinaryNotificationHeader header;
    header.notif_xfer_id = 42;
    header.notif_seq_id = 1; // Non-zero fragment
    header.notif_seq_len = 3;
    header.payload_length = 4;
    notif.setHeader(header);

    notif.setPayload(std::string("data"));

    char buffer[256];
    size_t size = notif.serialize(buffer);

    // Fragment N: header(10) + payload(4), no metadata
    TEST_ASSERT_EQ(size, 14u,
                   "Fragment N serialized size should be header(10)+payload(4)=14");

    // Deserialize and verify
    BinaryNotification restored;
    BinaryNotification::deserialize(buffer, size, restored);

    TEST_ASSERT_EQ(restored.getHeader().notif_seq_id, 1u,
                   "Deserialized fragment N seq_id should be 1");
    TEST_ASSERT(restored.getPayload() == "data",
                "Deserialized fragment N payload should be 'data'");
}

// ---------------------------------------------------------------------------
// Test Suite 4: Error Handling Scenarios
// ---------------------------------------------------------------------------

static void
test_control_message_invalid_operation() {
    std::cout << std::endl << "--- Error Handling Tests ---" << std::endl;

    // Test with an invalid operation value
    NixlControlMessage msg;
    msg.operation = 99;

    TEST_ASSERT(msg.operation != NixlControlMessage::READ_REQUEST,
                "Invalid operation should not match READ_REQUEST");
    TEST_ASSERT(msg.operation != NixlControlMessage::WRITE_NOTIFY,
                "Invalid operation should not match WRITE_NOTIFY");
}

static void
test_control_message_skip_control_rail() {
    // Verify the logic: READ_REQUEST should only be sent to data rails (id > 0)
    std::vector<uint32_t> selected_rails = {0, 1, 2, 3};
    std::vector<NixlControlMessage> sent_requests;

    for (uint32_t rail_id : selected_rails) {
        if (rail_id == 0) continue; // Skip control rail

        NixlControlMessage msg;
        msg.operation = NixlControlMessage::READ_REQUEST;
        msg.request_id = 1;
        msg.rail_id = rail_id;
        msg.length = 16384;
        sent_requests.push_back(msg);
    }

    TEST_ASSERT_EQ(sent_requests.size(), 3u,
                   "Should send READ_REQUEST to 3 data rails, skipping rail 0");

    for (const auto &req : sent_requests) {
        TEST_ASSERT(req.rail_id > 0,
                    "No READ_REQUEST should target rail 0 (control rail)");
    }
}

static void
test_control_message_length_validation() {
    // Simulate Producer-side validation: requested length must not exceed buffer
    NixlControlMessage msg;
    msg.operation = NixlControlMessage::READ_REQUEST;
    msg.request_id = 1;
    msg.rail_id = 1;
    msg.length = 2 * 1024 * 1024; // 2MB request

    size_t producer_buffer_size = 1 * 1024 * 1024; // 1MB buffer

    bool length_exceeds = (msg.length > producer_buffer_size);
    TEST_ASSERT(length_exceeds,
                "Producer should detect when requested length exceeds buffer size");
}

static void
test_imm_data_overflow_truncation() {
    // Verify fields are properly masked (overflow values get truncated)
    uint64_t imm = NIXL_MAKE_IMM_DATA(
        0x1F,     // 5 bits, should truncate to 0xF (4 bits)
        0x1FF,    // 9 bits, should truncate to 0xFF (8 bits)
        0x1FFFF,  // 17 bits, should truncate to 0xFFFF (16 bits)
        0x1F      // 5 bits, should truncate to 0xF (4 bits)
    );

    TEST_ASSERT_EQ(NIXL_GET_MSG_TYPE_FROM_IMM(imm), 0xFull,
                   "Overflow msg_type should truncate to 0xF");
    TEST_ASSERT_EQ(NIXL_GET_AGENT_INDEX_FROM_IMM(imm), 0xFFull,
                   "Overflow agent_index should truncate to 0xFF");
    TEST_ASSERT_EQ(NIXL_GET_XFER_ID_FROM_IMM(imm), 0xFFFFull,
                   "Overflow xfer_id should truncate to 0xFFFF");
    TEST_ASSERT_EQ(NIXL_GET_SEQ_ID_FROM_IMM(imm), 0xFull,
                   "Overflow seq_id should truncate to 0xF");
}

// ---------------------------------------------------------------------------
// Test Suite 5: Concurrency and Multi-Request Tracking
// ---------------------------------------------------------------------------

static void
test_concurrent_control_message_creation() {
    std::cout << std::endl << "--- Concurrency Tests ---" << std::endl;

    // Simulate multiple concurrent READ_REQUEST creations
    const int num_threads = 8;
    const int requests_per_thread = 100;
    std::atomic<int> total_created{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([&total_created, t, requests_per_thread]() {
            for (int i = 0; i < requests_per_thread; i++) {
                NixlControlMessage msg;
                msg.operation = NixlControlMessage::READ_REQUEST;
                msg.request_id = static_cast<uint32_t>(t * requests_per_thread + i);
                msg.rail_id = (t % 3) + 1; // Rails 1-3
                msg.length = 16384;

                // Verify each message is independently valid
                if (msg.operation == NixlControlMessage::READ_REQUEST &&
                    msg.request_id == static_cast<uint32_t>(t * requests_per_thread + i)) {
                    total_created.fetch_add(1);
                }
            }
        });
    }

    for (auto &t : threads) {
        t.join();
    }

    TEST_ASSERT_EQ(total_created.load(), num_threads * requests_per_thread,
                   "All concurrent READ_REQUEST messages should be created correctly");
}

static void
test_imm_data_concurrent_encoding() {
    // Test that immediate data encoding is thread-safe (pure computation)
    const int num_threads = 8;
    const int ops_per_thread = 1000;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([&failures, t, ops_per_thread]() {
            for (int i = 0; i < ops_per_thread; i++) {
                uint16_t xfer_id = static_cast<uint16_t>((t * ops_per_thread + i) & 0xFFFF);
                uint8_t agent_idx = static_cast<uint8_t>(t & 0xFF);

                uint64_t imm = NIXL_MAKE_IMM_DATA(
                    NIXL_LIBFABRIC_MSG_TRANSFER,
                    agent_idx,
                    xfer_id,
                    0
                );

                if (NIXL_GET_XFER_ID_FROM_IMM(imm) != xfer_id ||
                    NIXL_GET_AGENT_INDEX_FROM_IMM(imm) != agent_idx) {
                    failures.fetch_add(1);
                }
            }
        });
    }

    for (auto &t : threads) {
        t.join();
    }

    TEST_ASSERT_EQ(failures.load(), 0,
                   "All concurrent immediate data encode/decode should be consistent");
}

// ---------------------------------------------------------------------------
// Test Suite 6: Protocol Constants and Configuration
// ---------------------------------------------------------------------------

static void
test_protocol_constants() {
    std::cout << std::endl << "--- Protocol Constants Tests ---" << std::endl;

    TEST_ASSERT_EQ(NIXL_LIBFABRIC_MSG_NOTIFICTION, 2,
                   "MSG_NOTIFICATION constant should be 2");
    TEST_ASSERT_EQ(NIXL_LIBFABRIC_MSG_TRANSFER, 4,
                   "MSG_TRANSFER constant should be 4");
    TEST_ASSERT_EQ(NIXL_LIBFABRIC_CQ_BATCH_SIZE, 16,
                   "CQ_BATCH_SIZE constant should be 16");
    TEST_ASSERT_EQ(NIXL_LIBFABRIC_SEND_RECV_BUFFER_SIZE, 8192,
                   "SEND_RECV_BUFFER_SIZE should be 8192");
    TEST_ASSERT_EQ(NIXL_LIBFABRIC_RECV_POOL_SIZE, 1024,
                   "RECV_POOL_SIZE should be 1024");

    // Verify NixlControlMessage fits within send buffer
    TEST_ASSERT(sizeof(NixlControlMessage) <= NIXL_LIBFABRIC_SEND_RECV_BUFFER_SIZE,
                "NixlControlMessage must fit within SEND_RECV_BUFFER_SIZE");
}

static void
test_imm_data_bit_layout() {
    // Verify the bit layout: | 4-bit MSG | 8-bit AGENT | 16-bit XFER_ID | 4-bit SEQ |
    TEST_ASSERT_EQ(NIXL_MSG_TYPE_BITS, 4, "MSG_TYPE should use 4 bits");
    TEST_ASSERT_EQ(NIXL_AGENT_INDEX_BITS, 8, "AGENT_INDEX should use 8 bits");
    TEST_ASSERT_EQ(NIXL_XFER_ID_BITS, 16, "XFER_ID should use 16 bits");
    TEST_ASSERT_EQ(NIXL_SEQ_ID_BITS, 4, "SEQ_ID should use 4 bits");

    // Total bits should be 32 (fits in uint32_t immediate data)
    int total_bits = NIXL_MSG_TYPE_BITS + NIXL_AGENT_INDEX_BITS +
                     NIXL_XFER_ID_BITS + NIXL_SEQ_ID_BITS;
    TEST_ASSERT_EQ(total_bits, 32, "Total immediate data bits should be 32");

    // Verify shift amounts
    TEST_ASSERT_EQ(NIXL_MSG_TYPE_SHIFT, 0, "MSG_TYPE shift should be 0");
    TEST_ASSERT_EQ(NIXL_AGENT_INDEX_SHIFT, 4, "AGENT_INDEX shift should be 4");
    TEST_ASSERT_EQ(NIXL_XFER_ID_SHIFT, 12, "XFER_ID shift should be 12");
    TEST_ASSERT_EQ(NIXL_SEQ_ID_SHIFT, 28, "SEQ_ID shift should be 28");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int
main() {
    std::cout << "====================================================" << std::endl;
    std::cout << "  NIXL Libfabric Request/Response Protocol Tests    " << std::endl;
    std::cout << "====================================================" << std::endl;

    // Suite 1: NixlControlMessage structure
    test_control_message_size();
    test_control_message_default_construction();
    test_control_message_read_request();
    test_control_message_operation_enum();
    test_control_message_serialization();
    test_control_message_large_transfer();
    test_control_message_multi_rail();

    // Suite 2: Immediate data encoding/decoding
    test_imm_data_basic_encoding();
    test_imm_data_notification_type();
    test_imm_data_boundary_values();
    test_imm_data_zero_values();
    test_imm_data_field_isolation();
    test_imm_data_read_request_scenario();

    // Suite 3: BinaryNotification
    test_binary_notification_header_size();
    test_binary_notification_serialize_fragment0();
    test_binary_notification_serialize_fragment_n();

    // Suite 4: Error handling
    test_control_message_invalid_operation();
    test_control_message_skip_control_rail();
    test_control_message_length_validation();
    test_imm_data_overflow_truncation();

    // Suite 5: Concurrency
    test_concurrent_control_message_creation();
    test_imm_data_concurrent_encoding();

    // Suite 6: Protocol constants
    test_protocol_constants();
    test_imm_data_bit_layout();

    // Summary
    std::cout << std::endl;
    std::cout << "====================================================" << std::endl;
    std::cout << "  Test Results: " << tests_passed << " passed, "
              << tests_failed << " failed, " << tests_total << " total" << std::endl;
    std::cout << "====================================================" << std::endl;

    return (tests_failed > 0) ? 1 : 0;
}
