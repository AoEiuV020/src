// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/third_party/quiche/src/quic/core/http/quic_spdy_client_session.h"

#include <memory>
#include <string>
#include <vector>

#include "net/third_party/quiche/src/quic/core/crypto/null_decrypter.h"
#include "net/third_party/quiche/src/quic/core/crypto/null_encrypter.h"
#include "net/third_party/quiche/src/quic/core/http/quic_spdy_client_stream.h"
#include "net/third_party/quiche/src/quic/core/http/spdy_utils.h"
#include "net/third_party/quiche/src/quic/core/quic_utils.h"
#include "net/third_party/quiche/src/quic/core/tls_client_handshaker.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_arraysize.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_ptr_util.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_socket_address.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_str_cat.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_test.h"
#include "net/third_party/quiche/src/quic/test_tools/crypto_test_utils.h"
#include "net/third_party/quiche/src/quic/test_tools/mock_quic_spdy_client_stream.h"
#include "net/third_party/quiche/src/quic/test_tools/quic_config_peer.h"
#include "net/third_party/quiche/src/quic/test_tools/quic_connection_peer.h"
#include "net/third_party/quiche/src/quic/test_tools/quic_framer_peer.h"
#include "net/third_party/quiche/src/quic/test_tools/quic_packet_creator_peer.h"
#include "net/third_party/quiche/src/quic/test_tools/quic_session_peer.h"
#include "net/third_party/quiche/src/quic/test_tools/quic_spdy_session_peer.h"
#include "net/third_party/quiche/src/quic/test_tools/quic_test_utils.h"

using spdy::SpdyHeaderBlock;
using testing::_;
using testing::AnyNumber;
using testing::Invoke;
using testing::Truly;

namespace quic {
namespace test {
namespace {

const char kServerHostname[] = "test.example.com";
const uint16_t kPort = 443;

class TestQuicSpdyClientSession : public QuicSpdyClientSession {
 public:
  explicit TestQuicSpdyClientSession(
      const QuicConfig& config,
      const ParsedQuicVersionVector& supported_versions,
      QuicConnection* connection,
      const QuicServerId& server_id,
      QuicCryptoClientConfig* crypto_config,
      QuicClientPushPromiseIndex* push_promise_index)
      : QuicSpdyClientSession(config,
                              supported_versions,
                              connection,
                              server_id,
                              crypto_config,
                              push_promise_index) {}

  std::unique_ptr<QuicSpdyClientStream> CreateClientStream() override {
    return QuicMakeUnique<MockQuicSpdyClientStream>(
        GetNextOutgoingBidirectionalStreamId(), this, BIDIRECTIONAL);
  }

  MockQuicSpdyClientStream* CreateIncomingStream(QuicStreamId id) override {
    if (!ShouldCreateIncomingStream(id)) {
      return nullptr;
    }
    MockQuicSpdyClientStream* stream =
        new MockQuicSpdyClientStream(id, this, READ_UNIDIRECTIONAL);
    ActivateStream(QuicWrapUnique(stream));
    return stream;
  }
};

class QuicSpdyClientSessionTest : public QuicTestWithParam<ParsedQuicVersion> {
 protected:
  QuicSpdyClientSessionTest()
      : crypto_config_(crypto_test_utils::ProofVerifierForTesting(),
                       TlsClientHandshaker::CreateSslCtx()),
        promised_stream_id_(
            QuicUtils::GetInvalidStreamId(GetParam().transport_version)),
        associated_stream_id_(
            QuicUtils::GetInvalidStreamId(GetParam().transport_version)) {
    SetQuicFlag(FLAGS_quic_supports_tls_handshake, true);
    Initialize();
    // Advance the time, because timers do not like uninitialized times.
    connection_->AdvanceTime(QuicTime::Delta::FromSeconds(1));
  }

  ~QuicSpdyClientSessionTest() override {
    // Session must be destroyed before promised_by_url_
    session_.reset(nullptr);
  }

  void Initialize() {
    session_.reset();
    connection_ = new PacketSavingConnection(&helper_, &alarm_factory_,
                                             Perspective::IS_CLIENT,
                                             SupportedVersions(GetParam()));
    session_ = QuicMakeUnique<TestQuicSpdyClientSession>(
        DefaultQuicConfig(), SupportedVersions(GetParam()), connection_,
        QuicServerId(kServerHostname, kPort, false), &crypto_config_,
        &push_promise_index_);
    session_->Initialize();
    push_promise_[":path"] = "/bar";
    push_promise_[":authority"] = "www.google.com";
    push_promise_[":version"] = "HTTP/1.1";
    push_promise_[":method"] = "GET";
    push_promise_[":scheme"] = "https";
    promise_url_ = SpdyUtils::GetPromisedUrlFromHeaders(push_promise_);
    promised_stream_id_ = GetNthServerInitiatedUnidirectionalStreamId(
        connection_->transport_version(), 0);
    associated_stream_id_ = GetNthClientInitiatedBidirectionalStreamId(
        connection_->transport_version(), 0);
  }

  // The function ensures that A) the MAX_STREAMS frames get properly deleted
  // (since the test uses a 'did we leak memory' check ... if we just lose the
  // frame, the test fails) and B) returns true (instead of the default, false)
  // which ensures that the rest of the system thinks that the frame actually
  // was transmitted.
  bool ClearMaxStreamsControlFrame(const QuicFrame& frame) {
    if (frame.type == MAX_STREAMS_FRAME) {
      DeleteFrame(&const_cast<QuicFrame&>(frame));
      return true;
    }
    return false;
  }

 public:
  bool ClearStreamsBlockedControlFrame(const QuicFrame& frame) {
    if (frame.type == STREAMS_BLOCKED_FRAME) {
      DeleteFrame(&const_cast<QuicFrame&>(frame));
      return true;
    }
    return false;
  }

 protected:
  void CompleteCryptoHandshake() {
    CompleteCryptoHandshake(kDefaultMaxStreamsPerConnection);
  }

  void CompleteCryptoHandshake(uint32_t server_max_incoming_streams) {
    if (connection_->transport_version() == QUIC_VERSION_99) {
      EXPECT_CALL(*connection_, SendControlFrame(_))
          .Times(testing::AnyNumber())
          .WillRepeatedly(Invoke(
              this, &QuicSpdyClientSessionTest::ClearMaxStreamsControlFrame));
    }
    session_->CryptoConnect();
    QuicCryptoClientStream* stream = static_cast<QuicCryptoClientStream*>(
        session_->GetMutableCryptoStream());
    QuicConfig config = DefaultQuicConfig();
    if (connection_->transport_version() == QUIC_VERSION_99) {
      config.SetMaxIncomingUnidirectionalStreamsToSend(
          server_max_incoming_streams);
      config.SetMaxIncomingBidirectionalStreamsToSend(
          server_max_incoming_streams);
    } else {
      config.SetMaxIncomingBidirectionalStreamsToSend(
          server_max_incoming_streams);
    }
    crypto_test_utils::HandshakeWithFakeServer(
        &config, &helper_, &alarm_factory_, connection_, stream);
  }

  QuicCryptoClientConfig crypto_config_;
  MockQuicConnectionHelper helper_;
  MockAlarmFactory alarm_factory_;
  PacketSavingConnection* connection_;
  std::unique_ptr<TestQuicSpdyClientSession> session_;
  QuicClientPushPromiseIndex push_promise_index_;
  SpdyHeaderBlock push_promise_;
  std::string promise_url_;
  QuicStreamId promised_stream_id_;
  QuicStreamId associated_stream_id_;
};

INSTANTIATE_TEST_SUITE_P(Tests,
                         QuicSpdyClientSessionTest,
                         ::testing::ValuesIn(AllSupportedVersions()));

TEST_P(QuicSpdyClientSessionTest, CryptoConnect) {
  CompleteCryptoHandshake();
}

TEST_P(QuicSpdyClientSessionTest, NoEncryptionAfterInitialEncryption) {
  if (GetParam().handshake_protocol == PROTOCOL_TLS1_3) {
    // This test relies on resumption and is QUIC crypto specific, so it is
    // disabled for TLS.
    // TODO(nharper): Add support for resumption to the TLS handshake, and fix
    // this test to not rely on QUIC crypto.
    return;
  }
  // Complete a handshake in order to prime the crypto config for 0-RTT.
  CompleteCryptoHandshake();

  // Now create a second session using the same crypto config.
  Initialize();

  EXPECT_CALL(*connection_, OnCanWrite());
  // Starting the handshake should move immediately to encryption
  // established and will allow streams to be created.
  session_->CryptoConnect();
  EXPECT_TRUE(session_->IsEncryptionEstablished());
  QuicSpdyClientStream* stream = session_->CreateOutgoingBidirectionalStream();
  ASSERT_TRUE(stream != nullptr);
  EXPECT_FALSE(QuicUtils::IsCryptoStreamId(connection_->transport_version(),
                                           stream->id()));

  // Process an "inchoate" REJ from the server which will cause
  // an inchoate CHLO to be sent and will leave the encryption level
  // at NONE.
  CryptoHandshakeMessage rej;
  crypto_test_utils::FillInDummyReject(&rej);
  EXPECT_TRUE(session_->IsEncryptionEstablished());
  crypto_test_utils::SendHandshakeMessageToStream(
      session_->GetMutableCryptoStream(), rej, Perspective::IS_CLIENT);
  EXPECT_FALSE(session_->IsEncryptionEstablished());
  EXPECT_EQ(ENCRYPTION_INITIAL,
            QuicPacketCreatorPeer::GetEncryptionLevel(
                QuicConnectionPeer::GetPacketCreator(connection_)));
  // Verify that no new streams may be created.
  EXPECT_TRUE(session_->CreateOutgoingBidirectionalStream() == nullptr);
  // Verify that no data may be send on existing streams.
  char data[] = "hello world";
  QuicConsumedData consumed = session_->WritevData(
      stream, stream->id(), QUIC_ARRAYSIZE(data), 0, NO_FIN);
  EXPECT_FALSE(consumed.fin_consumed);
  EXPECT_EQ(0u, consumed.bytes_consumed);
}

TEST_P(QuicSpdyClientSessionTest, MaxNumStreamsWithNoFinOrRst) {
  if (GetParam().handshake_protocol == PROTOCOL_TLS1_3) {
    // This test relies on the MIDS transport parameter, which is not yet
    // supported in TLS 1.3.
    // TODO(nharper): Add support for Transport Parameters in the TLS handshake.
    return;
  }
  EXPECT_CALL(*connection_, SendControlFrame(_)).Times(AnyNumber());
  EXPECT_CALL(*connection_, OnStreamReset(_, _)).Times(AnyNumber());

  const uint32_t kServerMaxIncomingStreams = 1;
  CompleteCryptoHandshake(kServerMaxIncomingStreams);

  QuicSpdyClientStream* stream = session_->CreateOutgoingBidirectionalStream();
  ASSERT_TRUE(stream);
  EXPECT_FALSE(session_->CreateOutgoingBidirectionalStream());

  // Close the stream, but without having received a FIN or a RST_STREAM
  // or MAX_STREAMS (V99) and check that a new one can not be created.
  session_->CloseStream(stream->id());
  EXPECT_EQ(1u, session_->GetNumOpenOutgoingStreams());

  stream = session_->CreateOutgoingBidirectionalStream();
  EXPECT_FALSE(stream);

  if (GetParam().transport_version == QUIC_VERSION_99) {
    // Ensure that we have/have had 3 open streams, crypto, header, and the
    // 1 test stream. Primary purpose of this is to fail when crypto
    // no longer uses a normal stream. Some above constants will then need
    // to be changed.
    EXPECT_EQ(QuicSessionPeer::v99_bidirectional_stream_id_manager(&*session_)
                      ->outgoing_static_stream_count() +
                  1,
              QuicSessionPeer::v99_bidirectional_stream_id_manager(&*session_)
                  ->outgoing_stream_count());
  }
}

TEST_P(QuicSpdyClientSessionTest, MaxNumStreamsWithRst) {
  if (GetParam().handshake_protocol == PROTOCOL_TLS1_3) {
    // This test relies on the MIDS transport parameter, which is not yet
    // supported in TLS 1.3.
    // TODO(nharper): Add support for Transport Parameters in the TLS handshake.
    return;
  }
  EXPECT_CALL(*connection_, SendControlFrame(_)).Times(AnyNumber());
  EXPECT_CALL(*connection_, OnStreamReset(_, _)).Times(AnyNumber());

  const uint32_t kServerMaxIncomingStreams = 1;
  CompleteCryptoHandshake(kServerMaxIncomingStreams);

  QuicSpdyClientStream* stream = session_->CreateOutgoingBidirectionalStream();
  ASSERT_NE(nullptr, stream);
  EXPECT_EQ(nullptr, session_->CreateOutgoingBidirectionalStream());

  // Close the stream and receive an RST frame to remove the unfinished stream
  session_->CloseStream(stream->id());
  session_->OnRstStream(QuicRstStreamFrame(kInvalidControlFrameId, stream->id(),
                                           QUIC_RST_ACKNOWLEDGEMENT, 0));
  // Check that a new one can be created.
  EXPECT_EQ(0u, session_->GetNumOpenOutgoingStreams());
  if (GetParam().transport_version == QUIC_VERSION_99) {
    // In V99 the stream limit increases only if we get a MAX_STREAMS
    // frame; pretend we got one.

    // Note that this is to be the second stream created, hence
    // the stream count is 3 (the two streams created as a part of
    // the test plus the header stream, internally created).
    QuicMaxStreamsFrame frame(
        0,
        QuicSessionPeer::v99_bidirectional_stream_id_manager(&*session_)
                ->outgoing_static_stream_count() +
            2,
        /*unidirectional=*/false);
    session_->OnMaxStreamsFrame(frame);
  }
  stream = session_->CreateOutgoingBidirectionalStream();
  EXPECT_NE(nullptr, stream);
  if (GetParam().transport_version == QUIC_VERSION_99) {
    // Ensure that we have/have had three open streams: two test streams and the
    // header stream.
    EXPECT_EQ(3u,
              QuicSessionPeer::v99_bidirectional_stream_id_manager(&*session_)
                  ->outgoing_stream_count());
  }
}

TEST_P(QuicSpdyClientSessionTest, ResetAndTrailers) {
  if (GetParam().handshake_protocol == PROTOCOL_TLS1_3) {
    // This test relies on the MIDS transport parameter, which is not yet
    // supported in TLS 1.3.
    // TODO(nharper): Add support for Transport Parameters in the TLS handshake.
    return;
  }
  // Tests the situation in which the client sends a RST at the same time that
  // the server sends trailing headers (trailers). Receipt of the trailers by
  // the client should result in all outstanding stream state being tidied up
  // (including flow control, and number of available outgoing streams).
  const uint32_t kServerMaxIncomingStreams = 1;
  CompleteCryptoHandshake(kServerMaxIncomingStreams);

  QuicSpdyClientStream* stream = session_->CreateOutgoingBidirectionalStream();
  ASSERT_NE(nullptr, stream);

  if (GetParam().transport_version == QUIC_VERSION_99) {
    // For v99, trying to open a stream and failing due to lack
    // of stream ids will result in a STREAMS_BLOCKED. Make
    // sure we get one. Also clear out the frame because if it's
    // left sitting, the later SendRstStream will not actually
    // transmit the RST_STREAM because the connection will be in write-blocked
    // state. This means that the SendControlFrame that is expected w.r.t. the
    // RST_STREAM, below, will not be satisfied.
    EXPECT_CALL(*connection_, SendControlFrame(_))
        .WillOnce(Invoke(
            this, &QuicSpdyClientSessionTest::ClearStreamsBlockedControlFrame));
  }

  EXPECT_EQ(nullptr, session_->CreateOutgoingBidirectionalStream());

  QuicStreamId stream_id = stream->id();

  EXPECT_CALL(*connection_, SendControlFrame(_)).Times(1);
  EXPECT_CALL(*connection_, OnStreamReset(_, _)).Times(1);
  session_->SendRstStream(stream_id, QUIC_STREAM_PEER_GOING_AWAY, 0);

  // A new stream cannot be created as the reset stream still counts as an open
  // outgoing stream until closed by the server.
  EXPECT_EQ(1u, session_->GetNumOpenOutgoingStreams());
  stream = session_->CreateOutgoingBidirectionalStream();
  EXPECT_EQ(nullptr, stream);

  // The stream receives trailers with final byte offset: this is one of three
  // ways that a peer can signal the end of a stream (the others being RST,
  // stream data + FIN).
  QuicHeaderList trailers;
  trailers.OnHeaderBlockStart();
  trailers.OnHeader(kFinalOffsetHeaderKey, "0");
  trailers.OnHeaderBlockEnd(0, 0);
  session_->OnStreamHeaderList(stream_id, /*fin=*/false, 0, trailers);

  // The stream is now complete from the client's perspective, and it should
  // be able to create a new outgoing stream.
  EXPECT_EQ(0u, session_->GetNumOpenOutgoingStreams());
  if (GetParam().transport_version == QUIC_VERSION_99) {
    // Note that this is to be the second stream created, hence
    // the stream count is 3 (the two streams created as a part of
    // the test plus the header stream, internally created).
    QuicMaxStreamsFrame frame(
        0,
        QuicSessionPeer::v99_bidirectional_stream_id_manager(&*session_)
                ->outgoing_static_stream_count() +
            2,
        /*unidirectional=*/false);

    session_->OnMaxStreamsFrame(frame);
  }
  stream = session_->CreateOutgoingBidirectionalStream();
  EXPECT_NE(nullptr, stream);
  if (GetParam().transport_version == QUIC_VERSION_99) {
    // Ensure that we have/have had three open streams: two test streams and the
    // header stream.
    EXPECT_EQ(3u,
              QuicSessionPeer::v99_bidirectional_stream_id_manager(&*session_)
                  ->outgoing_stream_count());
  }
}

TEST_P(QuicSpdyClientSessionTest, ReceivedMalformedTrailersAfterSendingRst) {
  // Tests the situation where the client has sent a RST to the server, and has
  // received trailing headers with a malformed final byte offset value.
  CompleteCryptoHandshake();

  QuicSpdyClientStream* stream = session_->CreateOutgoingBidirectionalStream();
  ASSERT_NE(nullptr, stream);

  // Send the RST, which results in the stream being closed locally (but some
  // state remains while the client waits for a response from the server).
  QuicStreamId stream_id = stream->id();
  EXPECT_CALL(*connection_, SendControlFrame(_)).Times(1);
  EXPECT_CALL(*connection_, OnStreamReset(_, _)).Times(1);
  session_->SendRstStream(stream_id, QUIC_STREAM_PEER_GOING_AWAY, 0);

  // The stream receives trailers with final byte offset, but the header value
  // is non-numeric and should be treated as malformed.
  QuicHeaderList trailers;
  trailers.OnHeaderBlockStart();
  trailers.OnHeader(kFinalOffsetHeaderKey, "invalid non-numeric value");
  trailers.OnHeaderBlockEnd(0, 0);

  EXPECT_CALL(*connection_, CloseConnection(_, _, _)).Times(1);
  session_->OnStreamHeaderList(stream_id, /*fin=*/false, 0, trailers);
}

TEST_P(QuicSpdyClientSessionTest, OnStreamHeaderListWithStaticStream) {
  // Test situation where OnStreamHeaderList is called by stream with static id.
  CompleteCryptoHandshake();

  QuicHeaderList trailers;
  trailers.OnHeaderBlockStart();
  trailers.OnHeader(kFinalOffsetHeaderKey, "0");
  trailers.OnHeaderBlockEnd(0, 0);

  EXPECT_CALL(*connection_, CloseConnection(_, _, _)).Times(1);
  session_->OnStreamHeaderList(
      QuicUtils::GetHeadersStreamId(connection_->transport_version()),
      /*fin=*/false, 0, trailers);
}

TEST_P(QuicSpdyClientSessionTest, OnPromiseHeaderListWithStaticStream) {
  // Test situation where OnPromiseHeaderList is called by stream with static
  // id.
  CompleteCryptoHandshake();

  QuicHeaderList trailers;
  trailers.OnHeaderBlockStart();
  trailers.OnHeader(kFinalOffsetHeaderKey, "0");
  trailers.OnHeaderBlockEnd(0, 0);

  EXPECT_CALL(*connection_, CloseConnection(_, _, _)).Times(1);
  session_->OnPromiseHeaderList(
      QuicUtils::GetHeadersStreamId(connection_->transport_version()),
      promised_stream_id_, 0, trailers);
}

TEST_P(QuicSpdyClientSessionTest, GoAwayReceived) {
  if (connection_->transport_version() == QUIC_VERSION_99) {
    return;
  }
  CompleteCryptoHandshake();

  // After receiving a GoAway, I should no longer be able to create outgoing
  // streams.
  session_->connection()->OnGoAwayFrame(QuicGoAwayFrame(
      kInvalidControlFrameId, QUIC_PEER_GOING_AWAY, 1u, "Going away."));
  EXPECT_EQ(nullptr, session_->CreateOutgoingBidirectionalStream());
}

static bool CheckForDecryptionError(QuicFramer* framer) {
  return framer->error() == QUIC_DECRYPTION_FAILURE;
}

// Various sorts of invalid packets that should not cause a connection
// to be closed.
TEST_P(QuicSpdyClientSessionTest, InvalidPacketReceived) {
  QuicSocketAddress server_address(TestPeerIPAddress(), kTestPort);
  QuicSocketAddress client_address(TestPeerIPAddress(), kTestPort);

  EXPECT_CALL(*connection_, ProcessUdpPacket(server_address, client_address, _))
      .WillRepeatedly(Invoke(static_cast<MockQuicConnection*>(connection_),
                             &MockQuicConnection::ReallyProcessUdpPacket));
  EXPECT_CALL(*connection_, OnCanWrite()).Times(AnyNumber());
  EXPECT_CALL(*connection_, OnError(_)).Times(1);

  // Verify that empty packets don't close the connection.
  QuicReceivedPacket zero_length_packet(nullptr, 0, QuicTime::Zero(), false);
  EXPECT_CALL(*connection_, CloseConnection(_, _, _)).Times(0);
  session_->ProcessUdpPacket(client_address, server_address,
                             zero_length_packet);

  // Verifiy that small, invalid packets don't close the connection.
  char buf[2] = {0x00, 0x01};
  QuicConnectionId connection_id = session_->connection()->connection_id();
  QuicReceivedPacket valid_packet(buf, 2, QuicTime::Zero(), false);
  // Close connection shouldn't be called.
  EXPECT_CALL(*connection_, CloseConnection(_, _, _)).Times(0);
  if (connection_->transport_version() > QUIC_VERSION_44) {
    // Illegal fixed bit value.
    EXPECT_CALL(*connection_, OnError(_)).Times(1);
  }
  session_->ProcessUdpPacket(client_address, server_address, valid_packet);

  // Verify that a non-decryptable packet doesn't close the connection.
  QuicFramerPeer::SetLastSerializedServerConnectionId(
      QuicConnectionPeer::GetFramer(connection_), connection_id);
  ParsedQuicVersionVector versions = SupportedVersions(GetParam());
  QuicConnectionId destination_connection_id = EmptyQuicConnectionId();
  QuicConnectionId source_connection_id = connection_id;
  if (!GetQuicRestartFlag(quic_do_not_override_connection_id)) {
    destination_connection_id = connection_id;
    source_connection_id = EmptyQuicConnectionId();
  }
  std::unique_ptr<QuicEncryptedPacket> packet(ConstructEncryptedPacket(
      destination_connection_id, source_connection_id, false, false, 100,
      "data", CONNECTION_ID_ABSENT, CONNECTION_ID_ABSENT,
      PACKET_4BYTE_PACKET_NUMBER, &versions, Perspective::IS_SERVER));
  std::unique_ptr<QuicReceivedPacket> received(
      ConstructReceivedPacket(*packet, QuicTime::Zero()));
  // Change the last byte of the encrypted data.
  *(const_cast<char*>(received->data() + received->length() - 1)) += 1;
  EXPECT_CALL(*connection_, CloseConnection(_, _, _)).Times(0);
  EXPECT_CALL(*connection_, OnError(Truly(CheckForDecryptionError))).Times(1);
  session_->ProcessUdpPacket(client_address, server_address, *received);
}

// A packet with invalid framing should cause a connection to be closed.
TEST_P(QuicSpdyClientSessionTest, InvalidFramedPacketReceived) {
  if (GetParam().handshake_protocol == PROTOCOL_TLS1_3) {
    // TODO(nharper, b/112643533): Figure out why this test fails when TLS is
    // enabled and fix it.
    return;
  }
  QuicSocketAddress server_address(TestPeerIPAddress(), kTestPort);
  QuicSocketAddress client_address(TestPeerIPAddress(), kTestPort);
  if (GetParam().KnowsWhichDecrypterToUse()) {
    connection_->InstallDecrypter(
        ENCRYPTION_FORWARD_SECURE,
        QuicMakeUnique<NullDecrypter>(Perspective::IS_CLIENT));
  } else {
    connection_->SetDecrypter(
        ENCRYPTION_FORWARD_SECURE,
        QuicMakeUnique<NullDecrypter>(Perspective::IS_CLIENT));
  }

  EXPECT_CALL(*connection_, ProcessUdpPacket(server_address, client_address, _))
      .WillRepeatedly(Invoke(static_cast<MockQuicConnection*>(connection_),
                             &MockQuicConnection::ReallyProcessUdpPacket));
  EXPECT_CALL(*connection_, OnError(_)).Times(1);

  // Verify that a decryptable packet with bad frames does close the connection.
  QuicConnectionId destination_connection_id =
      session_->connection()->connection_id();
  QuicConnectionId source_connection_id = EmptyQuicConnectionId();
  QuicFramerPeer::SetLastSerializedServerConnectionId(
      QuicConnectionPeer::GetFramer(connection_), destination_connection_id);
  ParsedQuicVersionVector versions = {GetParam()};
  bool version_flag = false;
  QuicConnectionIdIncluded scid_included = CONNECTION_ID_ABSENT;
  if (VersionHasIetfInvariantHeader(GetParam().transport_version)) {
    version_flag = true;
    source_connection_id = destination_connection_id;
    scid_included = CONNECTION_ID_PRESENT;
  }
  std::unique_ptr<QuicEncryptedPacket> packet(ConstructMisFramedEncryptedPacket(
      destination_connection_id, source_connection_id, version_flag, false, 100,
      "data", CONNECTION_ID_ABSENT, scid_included, PACKET_4BYTE_PACKET_NUMBER,
      &versions, Perspective::IS_SERVER));
  std::unique_ptr<QuicReceivedPacket> received(
      ConstructReceivedPacket(*packet, QuicTime::Zero()));
  EXPECT_CALL(*connection_, CloseConnection(_, _, _)).Times(1);
  session_->ProcessUdpPacket(client_address, server_address, *received);
}

TEST_P(QuicSpdyClientSessionTest, PushPromiseOnPromiseHeaders) {
  // Initialize crypto before the client session will create a stream.
  CompleteCryptoHandshake();

  MockQuicSpdyClientStream* stream = static_cast<MockQuicSpdyClientStream*>(
      session_->CreateOutgoingBidirectionalStream());

  EXPECT_CALL(*stream, OnPromiseHeaderList(_, _, _));
  session_->OnPromiseHeaderList(associated_stream_id_, promised_stream_id_, 0,
                                QuicHeaderList());
}

TEST_P(QuicSpdyClientSessionTest, PushPromiseOnPromiseHeadersAlreadyClosed) {
  // Initialize crypto before the client session will create a stream.
  CompleteCryptoHandshake();

  session_->CreateOutgoingBidirectionalStream();

  EXPECT_CALL(*connection_, SendControlFrame(_));
  EXPECT_CALL(*connection_,
              OnStreamReset(associated_stream_id_, QUIC_REFUSED_STREAM));
  session_->ResetPromised(associated_stream_id_, QUIC_REFUSED_STREAM);

  session_->OnPromiseHeaderList(associated_stream_id_, promised_stream_id_, 0,
                                QuicHeaderList());
}

TEST_P(QuicSpdyClientSessionTest, PushPromiseOutOfOrder) {
  // Initialize crypto before the client session will create a stream.
  CompleteCryptoHandshake();

  MockQuicSpdyClientStream* stream = static_cast<MockQuicSpdyClientStream*>(
      session_->CreateOutgoingBidirectionalStream());

  EXPECT_CALL(*stream, OnPromiseHeaderList(promised_stream_id_, _, _));
  session_->OnPromiseHeaderList(associated_stream_id_, promised_stream_id_, 0,
                                QuicHeaderList());
  associated_stream_id_ +=
      QuicUtils::StreamIdDelta(connection_->transport_version());
  EXPECT_CALL(*connection_,
              CloseConnection(QUIC_INVALID_STREAM_ID,
                              "Received push stream id lesser or equal to the"
                              " last accepted before",
                              _));
  session_->OnPromiseHeaderList(associated_stream_id_, promised_stream_id_, 0,
                                QuicHeaderList());
}

TEST_P(QuicSpdyClientSessionTest, PushPromiseOutgoingStreamId) {
  // Initialize crypto before the client session will create a stream.
  CompleteCryptoHandshake();

  MockQuicSpdyClientStream* stream = static_cast<MockQuicSpdyClientStream*>(
      session_->CreateOutgoingBidirectionalStream());

  // Promise an illegal (outgoing) stream id.
  promised_stream_id_ = GetNthClientInitiatedBidirectionalStreamId(
      connection_->transport_version(), 0);
  EXPECT_CALL(
      *connection_,
      CloseConnection(QUIC_INVALID_STREAM_ID,
                      "Received push stream id for outgoing stream.", _));

  session_->OnPromiseHeaderList(stream->id(), promised_stream_id_, 0,
                                QuicHeaderList());
}

TEST_P(QuicSpdyClientSessionTest, PushPromiseHandlePromise) {
  // Initialize crypto before the client session will create a stream.
  CompleteCryptoHandshake();

  session_->CreateOutgoingBidirectionalStream();

  EXPECT_TRUE(session_->HandlePromised(associated_stream_id_,
                                       promised_stream_id_, push_promise_));

  EXPECT_NE(session_->GetPromisedById(promised_stream_id_), nullptr);
  EXPECT_NE(session_->GetPromisedByUrl(promise_url_), nullptr);
}

TEST_P(QuicSpdyClientSessionTest, PushPromiseAlreadyClosed) {
  // Initialize crypto before the client session will create a stream.
  CompleteCryptoHandshake();

  session_->CreateOutgoingBidirectionalStream();
  session_->GetOrCreateStream(promised_stream_id_);

  EXPECT_CALL(*connection_, SendControlFrame(_));
  EXPECT_CALL(*connection_,
              OnStreamReset(promised_stream_id_, QUIC_REFUSED_STREAM));

  session_->ResetPromised(promised_stream_id_, QUIC_REFUSED_STREAM);
  SpdyHeaderBlock promise_headers;
  EXPECT_FALSE(session_->HandlePromised(associated_stream_id_,
                                        promised_stream_id_, promise_headers));

  // Verify that the promise was not created.
  EXPECT_EQ(session_->GetPromisedById(promised_stream_id_), nullptr);
  EXPECT_EQ(session_->GetPromisedByUrl(promise_url_), nullptr);
}

TEST_P(QuicSpdyClientSessionTest, PushPromiseDuplicateUrl) {
  // Initialize crypto before the client session will create a stream.
  CompleteCryptoHandshake();

  session_->CreateOutgoingBidirectionalStream();

  EXPECT_TRUE(session_->HandlePromised(associated_stream_id_,
                                       promised_stream_id_, push_promise_));

  EXPECT_NE(session_->GetPromisedById(promised_stream_id_), nullptr);
  EXPECT_NE(session_->GetPromisedByUrl(promise_url_), nullptr);

  promised_stream_id_ +=
      QuicUtils::StreamIdDelta(connection_->transport_version());
  EXPECT_CALL(*connection_, SendControlFrame(_));
  EXPECT_CALL(*connection_,
              OnStreamReset(promised_stream_id_, QUIC_DUPLICATE_PROMISE_URL));

  EXPECT_FALSE(session_->HandlePromised(associated_stream_id_,
                                        promised_stream_id_, push_promise_));

  // Verify that the promise was not created.
  EXPECT_EQ(session_->GetPromisedById(promised_stream_id_), nullptr);
}

TEST_P(QuicSpdyClientSessionTest, ReceivingPromiseEnhanceYourCalm) {
  for (size_t i = 0u; i < session_->get_max_promises(); i++) {
    push_promise_[":path"] = QuicStringPrintf("/bar%zu", i);

    QuicStreamId id =
        promised_stream_id_ +
        i * QuicUtils::StreamIdDelta(connection_->transport_version());

    EXPECT_TRUE(
        session_->HandlePromised(associated_stream_id_, id, push_promise_));

    // Verify that the promise is in the unclaimed streams map.
    std::string promise_url(
        SpdyUtils::GetPromisedUrlFromHeaders(push_promise_));
    EXPECT_NE(session_->GetPromisedByUrl(promise_url), nullptr);
    EXPECT_NE(session_->GetPromisedById(id), nullptr);
  }

  // One more promise, this should be refused.
  int i = session_->get_max_promises();
  push_promise_[":path"] = QuicStringPrintf("/bar%d", i);

  QuicStreamId id =
      promised_stream_id_ +
      i * QuicUtils::StreamIdDelta(connection_->transport_version());
  EXPECT_CALL(*connection_, SendControlFrame(_));
  EXPECT_CALL(*connection_, OnStreamReset(id, QUIC_REFUSED_STREAM));
  EXPECT_FALSE(
      session_->HandlePromised(associated_stream_id_, id, push_promise_));

  // Verify that the promise was not created.
  std::string promise_url(SpdyUtils::GetPromisedUrlFromHeaders(push_promise_));
  EXPECT_EQ(session_->GetPromisedById(id), nullptr);
  EXPECT_EQ(session_->GetPromisedByUrl(promise_url), nullptr);
}

TEST_P(QuicSpdyClientSessionTest, IsClosedTrueAfterResetPromisedAlreadyOpen) {
  // Initialize crypto before the client session will create a stream.
  CompleteCryptoHandshake();

  session_->GetOrCreateStream(promised_stream_id_);
  EXPECT_CALL(*connection_, SendControlFrame(_));
  EXPECT_CALL(*connection_,
              OnStreamReset(promised_stream_id_, QUIC_REFUSED_STREAM));
  session_->ResetPromised(promised_stream_id_, QUIC_REFUSED_STREAM);
  EXPECT_TRUE(session_->IsClosedStream(promised_stream_id_));
}

TEST_P(QuicSpdyClientSessionTest, IsClosedTrueAfterResetPromisedNonexistant) {
  // Initialize crypto before the client session will create a stream.
  CompleteCryptoHandshake();

  EXPECT_CALL(*connection_, SendControlFrame(_));
  EXPECT_CALL(*connection_,
              OnStreamReset(promised_stream_id_, QUIC_REFUSED_STREAM));
  session_->ResetPromised(promised_stream_id_, QUIC_REFUSED_STREAM);
  EXPECT_TRUE(session_->IsClosedStream(promised_stream_id_));
}

TEST_P(QuicSpdyClientSessionTest, OnInitialHeadersCompleteIsPush) {
  // Initialize crypto before the client session will create a stream.
  CompleteCryptoHandshake();
  session_->GetOrCreateStream(promised_stream_id_);
  EXPECT_TRUE(session_->HandlePromised(associated_stream_id_,
                                       promised_stream_id_, push_promise_));
  EXPECT_NE(session_->GetPromisedById(promised_stream_id_), nullptr);
  EXPECT_NE(session_->GetPromisedStream(promised_stream_id_), nullptr);
  EXPECT_NE(session_->GetPromisedByUrl(promise_url_), nullptr);

  session_->OnInitialHeadersComplete(promised_stream_id_, SpdyHeaderBlock());
}

TEST_P(QuicSpdyClientSessionTest, OnInitialHeadersCompleteIsNotPush) {
  // Initialize crypto before the client session will create a stream.
  CompleteCryptoHandshake();
  session_->CreateOutgoingBidirectionalStream();
  session_->OnInitialHeadersComplete(promised_stream_id_, SpdyHeaderBlock());
}

TEST_P(QuicSpdyClientSessionTest, DeletePromised) {
  // Initialize crypto before the client session will create a stream.
  CompleteCryptoHandshake();
  session_->GetOrCreateStream(promised_stream_id_);
  EXPECT_TRUE(session_->HandlePromised(associated_stream_id_,
                                       promised_stream_id_, push_promise_));
  QuicClientPromisedInfo* promised =
      session_->GetPromisedById(promised_stream_id_);
  EXPECT_NE(promised, nullptr);
  EXPECT_NE(session_->GetPromisedStream(promised_stream_id_), nullptr);
  EXPECT_NE(session_->GetPromisedByUrl(promise_url_), nullptr);

  session_->DeletePromised(promised);
  EXPECT_EQ(session_->GetPromisedById(promised_stream_id_), nullptr);
  EXPECT_EQ(session_->GetPromisedByUrl(promise_url_), nullptr);
}

TEST_P(QuicSpdyClientSessionTest, ResetPromised) {
  // Initialize crypto before the client session will create a stream.
  CompleteCryptoHandshake();
  session_->GetOrCreateStream(promised_stream_id_);
  EXPECT_TRUE(session_->HandlePromised(associated_stream_id_,
                                       promised_stream_id_, push_promise_));
  EXPECT_CALL(*connection_, SendControlFrame(_));
  EXPECT_CALL(*connection_,
              OnStreamReset(promised_stream_id_, QUIC_STREAM_PEER_GOING_AWAY));
  session_->SendRstStream(promised_stream_id_, QUIC_STREAM_PEER_GOING_AWAY, 0);
  QuicClientPromisedInfo* promised =
      session_->GetPromisedById(promised_stream_id_);
  EXPECT_NE(promised, nullptr);
  EXPECT_NE(session_->GetPromisedByUrl(promise_url_), nullptr);
  EXPECT_EQ(session_->GetPromisedStream(promised_stream_id_), nullptr);
}

TEST_P(QuicSpdyClientSessionTest, PushPromiseInvalidMethod) {
  // Initialize crypto before the client session will create a stream.
  CompleteCryptoHandshake();

  session_->CreateOutgoingBidirectionalStream();

  EXPECT_CALL(*connection_, SendControlFrame(_));
  EXPECT_CALL(*connection_,
              OnStreamReset(promised_stream_id_, QUIC_INVALID_PROMISE_METHOD));

  push_promise_[":method"] = "POST";
  EXPECT_FALSE(session_->HandlePromised(associated_stream_id_,
                                        promised_stream_id_, push_promise_));

  EXPECT_EQ(session_->GetPromisedById(promised_stream_id_), nullptr);
  EXPECT_EQ(session_->GetPromisedByUrl(promise_url_), nullptr);
}

TEST_P(QuicSpdyClientSessionTest, PushPromiseInvalidHost) {
  // Initialize crypto before the client session will create a stream.
  CompleteCryptoHandshake();

  session_->CreateOutgoingBidirectionalStream();

  EXPECT_CALL(*connection_, SendControlFrame(_));
  EXPECT_CALL(*connection_,
              OnStreamReset(promised_stream_id_, QUIC_INVALID_PROMISE_URL));

  push_promise_[":authority"] = "";
  EXPECT_FALSE(session_->HandlePromised(associated_stream_id_,
                                        promised_stream_id_, push_promise_));

  EXPECT_EQ(session_->GetPromisedById(promised_stream_id_), nullptr);
  EXPECT_EQ(session_->GetPromisedByUrl(promise_url_), nullptr);
}

TEST_P(QuicSpdyClientSessionTest,
       TryToCreateServerInitiatedBidirectionalStream) {
  if (connection_->transport_version() == QUIC_VERSION_99) {
    EXPECT_CALL(*connection_, CloseConnection(QUIC_INVALID_STREAM_ID, _, _));
  } else {
    EXPECT_CALL(*connection_, CloseConnection(_, _, _)).Times(0);
  }
  session_->GetOrCreateStream(GetNthServerInitiatedBidirectionalStreamId(
      connection_->transport_version(), 0));
}

}  // namespace
}  // namespace test
}  // namespace quic
