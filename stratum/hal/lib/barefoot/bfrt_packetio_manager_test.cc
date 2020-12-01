// Copyright 2020-present Open Networking Foundation
// SPDX-License-Identifier: Apache-2.0

#include "stratum/hal/lib/barefoot/bfrt_packetio_manager.h"

#include "absl/memory/memory.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "p4/v1/p4runtime.pb.h"
#include "stratum/glue/status/status_test_util.h"
#include "stratum/hal/lib/barefoot/bf_sde_mock.h"
#include "stratum/lib/utils.h"

using ::testing::_;
using ::testing::DoAll;
using ::testing::HasSubstr;
using ::testing::Invoke;
using ::testing::InvokeWithoutArgs;
using ::testing::Return;

namespace stratum {
namespace hal {
namespace barefoot {

class BfrtPacketioManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bf_sde_wrapper_mock_ = absl::make_unique<BfSdeMock>();
    bfrt_packetio_manager_ = BfrtPacketioManager::CreateInstance(
        kDevice1, bf_sde_wrapper_mock_.get());
  }

  ::util::Status PushPipelineConfig() {
    BfrtDeviceConfig config;
    auto* program = config.add_programs();
    EXPECT_OK(ParseProtoFromString(kP4Info, program->mutable_p4info()));
    // What we expected when calling PushForwardingPipelineConfig with valid
    // config:
    // - StartPacketIo of SDE interface will be invoked
    EXPECT_CALL(*bf_sde_wrapper_mock_, StartPacketIo(kDevice1))
        .WillOnce(Return(util::OkStatus()));
    // - RegisterPacketReceiveWriter of SDE interface will be invoked
    EXPECT_CALL(*bf_sde_wrapper_mock_, RegisterPacketReceiveWriter(kDevice1, _))
        .WillOnce(Return(util::OkStatus()));
    return bfrt_packetio_manager_->PushForwardingPipelineConfig(config);
  }

  ::util::Status Shutdown() {
    // Make sure everything like Rx threads will be cleaned up.
    EXPECT_CALL(*bf_sde_wrapper_mock_, StopPacketIo(kDevice1))
        .WillOnce(Return(util::OkStatus()));
    EXPECT_CALL(*bf_sde_wrapper_mock_, UnregisterPacketReceiveWriter(kDevice1))
        .WillOnce(Return(util::OkStatus()));
    return bfrt_packetio_manager_->Shutdown();
  }

  static constexpr int kDevice1 = 0;
  static constexpr char kP4Info[] = R"PROTO(
    controller_packet_metadata {
      preamble {
        id: 67146229
        name: "packet_in"
        alias: "packet_in"
        annotations: "@controller_header(\"packet_in\")"
      }
      metadata {
        id: 1
        name: "ingress_port"
        bitwidth: 9
      }
      metadata {
        id: 2
        name: "_pad0"
        bitwidth: 7
      }
    }
    controller_packet_metadata {
      preamble {
        id: 67121543
        name: "packet_out"
        alias: "packet_out"
        annotations: "@controller_header(\"packet_out\")"
      }
      metadata {
        id: 1
        name: "egress_port"
        bitwidth: 9
      }
      metadata {
        id: 2
        name: "cpu_loopback_mode"
        bitwidth: 2
      }
      metadata {
        id: 3
        name: "pad0"
        annotations: "@padding"
        bitwidth: 85
      }
      metadata {
        id: 4
        name: "ether_type"
        bitwidth: 16
      }
    }
  )PROTO";

  std::unique_ptr<BfSdeMock> bf_sde_wrapper_mock_;
  std::unique_ptr<BfrtPacketioManager> bfrt_packetio_manager_;
  std::shared_ptr<WriterInterface<::p4::v1::PacketIn>> packet_rx_writer;
};

// Basic set up and shutdown test

// TODO(Yi Tseng): These two methods will always return OK status
// We can add tests for these methods if we modify them.
// TEST_F(BfrtPacketioManagerTest, PushChassisConfig) {}
// TEST_F(BfrtPacketioManagerTest, VerifyChassisConfig) {}

constexpr int BfrtPacketioManagerTest::kDevice1;
constexpr char BfrtPacketioManagerTest::kP4Info[];

TEST_F(BfrtPacketioManagerTest, PushForwardingPipelineConfigAndShutdown) {
  EXPECT_OK(PushPipelineConfig());
  EXPECT_OK(Shutdown());
}

TEST_F(BfrtPacketioManagerTest, TransmitPacketAfterPipelineConfigPush) {
  EXPECT_OK(PushPipelineConfig());
  p4::v1::PacketOut packet_out;
  const char packet_out_str[] = R"PROTO(
    payload: "abcde"
    metadata {
      metadata_id: 1
      value: "\x1"
    }
    metadata {
      metadata_id: 2
      value: "\x0"
    }
    metadata {
      metadata_id: 3
      value: "\x0"
    }
    metadata {
      metadata_id: 4
      value: "\xbf\x01"
    }
  )PROTO";
  EXPECT_OK(ParseProtoFromString(packet_out_str, &packet_out));
  const std::string expected_packet(
      "\0\x80\0\0\0\0\0\0\0\0\0\0\xBF\x1"
      "abcde",
      19);
  EXPECT_CALL(*bf_sde_wrapper_mock_, TxPacket(kDevice1, expected_packet))
      .WillOnce(Return(util::OkStatus()));
  EXPECT_OK(bfrt_packetio_manager_->TransmitPacket(packet_out));
  EXPECT_OK(Shutdown());
}

TEST_F(BfrtPacketioManagerTest, TransmitBadPacketAfterPipelineConfigPush) {
  EXPECT_OK(PushPipelineConfig());
  p4::v1::PacketOut packet_out;
  // Missing the third metadata.
  const char packet_out_str[] = R"PROTO(
    payload: "abcde"
    metadata {
      metadata_id: 1
      value: "\x1"
    }
    metadata {
      metadata_id: 2
      value: "\x0"
    }
    metadata {
      metadata_id: 3
      value: "\x0"
    }
  )PROTO";
  EXPECT_OK(ParseProtoFromString(packet_out_str, &packet_out));
  auto status = bfrt_packetio_manager_->TransmitPacket(packet_out);
  EXPECT_FALSE(status.ok());
  EXPECT_THAT(status.error_message(),
              HasSubstr("Missing metadata with Id 4 in PacketOut"));
  EXPECT_OK(Shutdown());
}

}  // namespace barefoot
}  // namespace hal
}  // namespace stratum