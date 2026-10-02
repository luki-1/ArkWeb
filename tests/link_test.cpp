// Offline tests for the ArkWeb link: coordinate mapping, seqlock under contention, SPSC ring
// integrity, and the protocol layout (--layout prints it as JSON for tools/arkweb_proto.py).
#include "../src/common/coords.h"
#include "../src/common/link.h"

#include <cstddef>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

using namespace arkweb;

static int g_fail = 0;
#define CHECK(c)                                                         \
	do {                                                                 \
		if (!(c)) {                                                      \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);   \
			++g_fail;                                                    \
		}                                                                \
	} while (0)

static bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

static void TestCoords()
{
	using namespace coords;
	// Round trips.
	for (V3 p : { V3{ -68927.6, -57262.8, 3894.0 }, V3{ 0, 0, 0 }, V3{ 12345, -54321, -999 } }) {
		V3 g = HostPosToGuest(p), h = GuestPosToHost(g);
		CHECK(Near(h.x, p.x, 1e-6) && Near(h.y, p.y, 1e-6) && Near(h.z, p.z, 1e-6));
	}
	// Up is up, scale is 100 UU per meter.
	V3 up = HostPosToGuest({ 0, 0, 190 });
	CHECK(Near(up.y, 1.9) && Near(up.x, 0) && Near(up.z, 0));
	// Host yaw <-> guest forward round trip through the pawn path.
	for (int yaw : { 0, 16384, 32768, 49152, 65536 - 10194 }) {
		V3 gf = HostDirToGuest(HostForwardOfYaw(yaw));
		int back = GuestForwardToHostYaw(gf);
		int diff = (back - yaw) & 0xFFFF;
		CHECK(diff <= 1 || diff >= 0xFFFF);
	}
	// Guest yaw convention: forward = (sin, 0, cos).
	for (double y : { 0.0, 1.0, -2.5, 3.0 }) CHECK(Near(GuestYawOfForward(GuestForwardOfYaw(y)), y, 1e-9));
	// Handedness: host (LH) right = up x forward with the LH rule; the mapped guest vectors must
	// keep the physical relation, i.e. guest right = forward x up in a right-handed guest.
	V3 hf{ 1, 0, 0 }, hr{ 0, 1, 0 }, hu{ 0, 0, 1 };  // UE3: X fwd, Y right, Z up
	V3 gf = HostDirToGuest(hf), gr = HostDirToGuest(hr), gu = HostDirToGuest(hu);
	V3 c{ gf.y * gu.z - gf.z * gu.y, gf.z * gu.x - gf.x * gu.z, gf.x * gu.y - gf.y * gu.x };  // fwd x up
	if (kGuestRightHanded) CHECK(Near(c.x, gr.x) && Near(c.y, gr.y) && Near(c.z, gr.z));
	else CHECK(Near(c.x, -gr.x) && Near(c.y, -gr.y) && Near(c.z, -gr.z));
}

static void TestSeqlock(Link& a_link)
{
	std::atomic<bool> stop{ false };
	std::thread       writer([&] {
        proto::GuestState s{};
        for (uint64_t i = 1; !stop; ++i) {
            s.frame = i;
            for (int k = 0; k < 3; ++k) s.heroPos[k] = static_cast<double>(i);
            for (int k = 0; k < 9; ++k) s.heroRot[k] = static_cast<float>(i);
            SeqWrite(a_link.Guest(), s);
        }
    });
	int torn = 0, reads = 0, misses = 0;
	uint64_t last = 0;
	for (int i = 0; i < 2000000; ++i) {
		proto::GuestState r;
		if (!SeqRead(a_link.Guest(), r)) {
			++misses;
			continue;
		}
		++reads;
		double v = static_cast<double>(r.frame);
		bool consistent = r.heroPos[0] == v && r.heroPos[2] == v && r.heroRot[8] == static_cast<float>(r.frame) && r.frame >= last;
		if (!consistent) ++torn;
		last = r.frame;
	}
	stop = true;
	writer.join();
	std::printf("seqlock: %d reads, %d gave up, %d torn\n", reads, misses, torn);
	CHECK(torn == 0 && reads > 0);
}

static void TestRing()
{
	std::vector<uint8_t> mem(0x80 + 4096, 0);
	Ring w(mem.data(), mem.size()), r(mem.data(), mem.size());
	constexpr int kRecords = 200000;
	std::atomic<bool> done{ false };
	std::thread producer([&] {
		std::mt19937 rng(1);
		std::vector<uint8_t> buf(700);
		for (uint32_t i = 0; i < kRecords; ++i) {
			uint32_t n = rng() % 700;
			for (uint32_t k = 0; k < n; ++k) buf[k] = static_cast<uint8_t>(i + k);
			if (n >= 4) memcpy(buf.data(), &i, 4);
			while (!w.Write(1 + (i % 3), buf.data(), n)) std::this_thread::yield();
		}
		done = true;
	});
	std::mt19937 rng(1);
	uint32_t expect = 0;
	int bad = 0;
	while (expect < kRecords) {
		int got = r.Drain([&](uint32_t a_type, const uint8_t* a_p, uint32_t a_n) {
			uint32_t n = rng() % 700;
			bool ok = a_type == 1 + (expect % 3) && a_n == n;
			for (uint32_t k = (n >= 4 ? 4 : 0); ok && k < n; ++k) ok = a_p[k] == static_cast<uint8_t>(expect + k);
			if (ok && n >= 4) ok = memcmp(a_p, &expect, 4) == 0;
			if (!ok) ++bad;
			++expect;
		});
		if (!got) std::this_thread::yield();
	}
	producer.join();
	std::printf("ring: %u records through a 4 KB ring, %d bad\n", expect, bad);
	CHECK(bad == 0 && r.Pending() == 0);
}

static void PrintLayout()
{
#define F(T, f) std::printf("  \"%s.%s\": %zu,\n", #T, #f, offsetof(proto::T, f))
	std::printf("{\n");
	F(Header, magic); F(Header, version); F(Header, hostPid); F(Header, guestPid); F(Header, hostHeartbeatMs); F(Header, guestHeartbeatMs);
	F(HostState, seq); F(HostState, flags); F(HostState, frame); F(HostState, deltaTime); F(HostState, teleportSeq); F(HostState, teleportPos);
	F(HostState, teleportYaw); F(HostState, worldId); F(HostState, puppetPos); F(HostState, puppetYaw); F(HostState, collisionEpoch);
	F(PadState, seq); F(PadState, flags); F(PadState, packet); F(PadState, buttons); F(PadState, leftTrigger); F(PadState, rightTrigger);
	F(PadState, thumbLX); F(PadState, thumbLY); F(PadState, thumbRX); F(PadState, thumbRY); F(PadState, camYaw); F(PadState, camPitch);
	F(GuestState, seq); F(GuestState, flags); F(GuestState, frame); F(GuestState, qpc); F(GuestState, heroPos); F(GuestState, heroRot);
	F(GuestState, heroVel); F(GuestState, heroHeight); F(GuestState, teleportAck); F(GuestState, camPos); F(GuestState, camRot); F(GuestState, camFovYDeg);
	F(GuestState, tilesHeld); F(GuestState, tilesQueued); F(GuestState, heapFreeMB); F(GuestState, heapUsedMB); F(GuestState, rescues);
	F(GuestState, buildsRefused); F(GuestState, zipFlags); F(GuestState, zipEdges); F(GuestState, zipTarget); F(GuestState, zipStarted);
	F(GuestState, zipFailed);
	F(CaptureState, seq); F(CaptureState, flags); F(CaptureState, generation); F(CaptureState, slot); F(CaptureState, frameId); F(CaptureState, qpc);
	F(CaptureState, width); F(CaptureState, height); F(CaptureState, tanHalfFovX); F(CaptureState, tanHalfFovY); F(CaptureState, camPos);
	F(CaptureState, camRot); F(CaptureState, range); F(CaptureState, format); F(CaptureState, share); F(CaptureState, handles);
	F(PoseState, seq); F(PoseState, flags); F(PoseState, frame); F(PoseState, count); F(PoseState, bone);
	F(HostState, viewPos); F(HostState, viewRot); F(HostState, viewFovX);
	F(TileRecord, key); F(TileRecord, awtBytes); F(TileRecord, awhBytes);
	std::printf("  \"kOffHostRing\": %llu, \"kHostRingBytes\": %llu, \"kOffCollisionRing\": %llu, \"kCollisionRingBytes\": %llu,\n", proto::kOffHostRing,
		proto::kHostRingBytes, proto::kOffCollisionRing, proto::kCollisionRingBytes);
	std::printf("  \"kOffHostState\": %llu, \"kOffPad\": %llu, \"kOffGuestState\": %llu, \"kOffCapture\": %llu, \"kOffPose\": %llu, \"kMappingBytes\": %llu, \"kVersion\": %u\n}\n",
		proto::kOffHostState, proto::kOffPad, proto::kOffGuestState, proto::kOffCapture, proto::kOffPose, proto::kMappingBytes, proto::kVersion);
#undef F
}

int main(int a_argc, char** a_argv)
{
	if (a_argc > 1 && !strcmp(a_argv[1], "--layout")) {
		PrintLayout();
		return 0;
	}
	TestCoords();
	Link link;
	CHECK(link.Open());
	if (link.Valid()) TestSeqlock(link);
	TestRing();
	std::printf(g_fail ? "%d FAILED\n" : "all tests passed\n", g_fail);
	return g_fail != 0;
}
