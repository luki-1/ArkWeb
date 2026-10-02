// ArkWeb shared-memory protocol: Batman: Arkham Knight (host) <-> Marvel's Spider-Man Remastered
// (guest). Same design as SkyCraft: latest-value slots under a seqlock for per-frame state, SPSC
// byte rings for streams.
//
// This header is the single source of truth for the byte layout. tools/arkweb_proto.py mirrors it
// (checked by tests/link_test.exe --layout); change both and bump kVersion.
//
// All values are little-endian. Whichever side starts first creates the mapping; both use
// CreateFileMapping/OpenFileMapping with kMappingName. Positions and directions are always
// GUEST space (Spider-Man: meters, Y up) unless a field says otherwise; the host converts with
// src/common/coords.h.
#pragma once

#include <cstdint>

namespace arkweb::proto
{
	inline constexpr std::uint32_t kMagic = 0x574B5241;  // "ARKW"
	inline constexpr std::uint32_t kVersion = 6;
	inline constexpr wchar_t       kMappingName[] = L"Local\\ArkWeb_v1";
	inline constexpr char          kMappingNameA[] = "Local\\ArkWeb_v1";

	// ---- region offsets ---------------------------------------------------------------------
	inline constexpr std::uint64_t kOffHeader = 0x0;
	inline constexpr std::uint64_t kOffHostState = 0x100;
	inline constexpr std::uint64_t kOffPad = 0x200;
	inline constexpr std::uint64_t kOffGuestState = 0x300;
	inline constexpr std::uint64_t kOffCapture = 0x800;     // guest -> host: Spider-Man's frame (CaptureState)
	// kOffPose = 0xA00: host -> guest, Batman's pose (PoseState, defined with it below)
	inline constexpr std::uint64_t kOffEventRing = 0x1000;  // guest -> host events (later phases)
	inline constexpr std::uint64_t kEventRingBytes = 0x10000;
	inline constexpr std::uint64_t kOffHostRing = 0x11000;  // streamer -> host: commands (kRecCommand)
	inline constexpr std::uint64_t kHostRingBytes = 0xF000;
	inline constexpr std::uint64_t kOffCollisionRing = 0x20000;  // streamer -> guest: commands + Gotham tiles
	inline constexpr std::uint64_t kCollisionRingBytes = 32ull << 20;
	inline constexpr std::uint64_t kMappingBytes = kOffCollisionRing + kCollisionRingBytes;

	// ---- header @0x0 --------------------------------------------------------------------------
	struct Header
	{
		std::uint32_t magic;
		std::uint32_t version;
		std::uint32_t hostPid;
		std::uint32_t guestPid;
		std::uint64_t hostHeartbeatMs;   // GetTickCount64() at the host's last frame
		std::uint64_t guestHeartbeatMs;  // GetTickCount64() at the guest's last publish
	};
	static_assert(sizeof(Header) == 0x20);

	inline constexpr std::uint64_t kHeartbeatTimeoutMs = 1500;  // older: that side is gone

	// ---- host -> guest state @0x100 (seqlock: seq odd while writing) ---------------------------
	enum HostFlags : std::uint32_t
	{
		kHostInGame = 1u << 0,       // a save is loaded and Batman exists
		kHostMenuOpen = 1u << 1,     // an AK menu owns input; the guest should drop held input
		kHostLoading = 1u << 2,      // loading screen / streaming stall
		kHostDrivePuppet = 1u << 3,  // the host is applying GuestState to Batman
		kHostCombat = 1u << 4,       // Arkham says Batman is fighting: Arkham has him, the guest pins Spider-Man to
		                             // him (puppetPos, puppetYaw) and mirrors his pose (PoseState)
		kHostView = 1u << 5,         // combat: Arkham's own camera is the view (viewPos/Rot/FovX); Spider-Man's camera
		                             // copies it, so the captured Spider-Man lines up with Arkham's picture
	};

	struct HostState
	{
		std::uint32_t seq;
		std::uint32_t flags;          // HostFlags
		std::uint64_t frame;          // host game-thread ticks
		float         deltaTime;      // seconds, last host tick
		std::uint32_t teleportSeq;    // guest teleports its hero to teleportPos when this changes
		double        teleportPos[3]; // guest space, hero feet
		float         teleportYaw;    // radians about +Y, 0 = facing +Z
		std::uint32_t worldId;        // host level/world identifier (changes drop collision)
		double        puppetPos[3];   // where Batman's feet are right now, guest space
		float         puppetYaw;      // radians, guest convention
		std::uint32_t collisionEpoch; // bumps when the host clears all collision (Phase 2)
		double        viewPos[3];     // kHostView: Arkham's camera this frame, guest space (anchored)
		float         viewRot[9];     // rows: side (screen left), up, forward
		float         viewFovX;       // degrees, horizontal
		std::uint8_t  reserved[0x100 - 0x98];
	};
	static_assert(sizeof(HostState) == 0x100);

	// ---- virtual gamepad @0x200 (host or tools -> guest; seqlock) ------------------------------
	// XInput layout so the guest can return it from XInputGetState unchanged.
	enum PadFlags : std::uint32_t
	{
		kPadActive = 1u << 0,  // guest reports this pad as connected controller 0
		kPadCamera = 1u << 1,  // camYaw / camPitch are valid: the guest steers its camera to them
		kPadAnalog = 1u << 2,  // a real controller: the guest serves this as its XInput pad (no key injection)
	};

	struct PadState
	{
		std::uint32_t seq;
		std::uint32_t flags;      // PadFlags
		std::uint32_t packet;     // increments with every change (XINPUT_STATE.dwPacketNumber)
		std::uint16_t buttons;    // XINPUT_GAMEPAD_* bits
		std::uint8_t  leftTrigger;
		std::uint8_t  rightTrigger;
		std::int16_t  thumbLX, thumbLY, thumbRX, thumbRY;
		float         camYaw;    // host camera direction in guest yaw (radians, GuestYawOfForward convention)
		float         camPitch;  // radians, up positive
		std::uint8_t  reserved[0x100 - 0x20];
	};
	static_assert(sizeof(PadState) == 0x100);

	// ---- guest -> host state @0x300 (seqlock) --------------------------------------------------
	enum GuestFlags : std::uint32_t
	{
		kGuestInWorld = 1u << 0,    // the hero exists and the transform is valid
		kGuestFocusFaked = 1u << 1, // the guest is running with its focus hooks engaged
		kGuestAnchored = 1u << 2,   // heroPos is in Arkham's frame (Gotham loaded with an .origin)
		kGuestSky = 1u << 3,        // Gotham is placed high above New York and the hero jumped up onto it
		kGuestJumpFailed = 1u << 4, // the jump onto Gotham didn't hold (the streamer falls back to ground placement)
	};

	struct GuestState
	{
		std::uint32_t seq;
		std::uint32_t flags;          // GuestFlags
		std::uint64_t frame;          // guest publishes
		std::int64_t  qpc;            // QueryPerformanceCounter when the transform was read
		double        heroPos[3];     // hero feet (actor origin), guest space
		float         heroRot[9];     // rows: side, up, forward (guest matrix rows 0..2)
		float         heroVel[3];     // m/s, guest space (finite difference of heroPos)
		float         heroHeight;     // meters (bounds), for the host's collision cylinder
		std::uint32_t teleportAck;    // last HostState::teleportSeq applied
		// Camera (Phase 4; zero until the guest exports it).
		double        camPos[3];
		float         camRot[9];      // rows: side, up, forward
		float         camFovYDeg;
		// Gotham streaming feedback (for tools/gotham_stream.py)
		std::uint32_t tilesHeld;      // streamed tiles the guest holds (built or waiting)
		std::uint32_t tilesQueued;    // of those, waiting to be built
		std::uint32_t heapFreeMB;     // Havok heap left (Spider-Man's physics heap is a fixed size)
		std::uint32_t heapUsedMB;     // Havok heap in use
		std::uint32_t rescues;        // times the guest put a hero who fell through Gotham back on it
		std::uint32_t buildsRefused;  // tile parts not built for lack of Havok heap
		// Zip to point (Arkham's grapple ledges as Spider-Man point-launch targets)
		std::uint32_t zipFlags;       // ZipFlags
		std::uint32_t zipEdges;       // grapple ledges the guest holds
		double        zipTarget[3];   // the targeted point, guest space (anchored like heroPos)
		std::uint32_t zipStarted;     // point launches started so far
		std::uint32_t zipFailed;      // point launch requests the game refused (or couldn't be made)
		std::uint8_t  reserved[0x200 - 0xE8];
	};
	static_assert(sizeof(GuestState) == 0x200);

	enum ZipFlags : std::uint32_t
	{
		kZipTarget = 1u << 0,  // zipTarget is valid: L2 + R2 point-launches there (the host draws a marker)
		kZipActive = 1u << 1,  // a point launch the guest started is under way
		kZipReady = 1u << 2,   // the guest found Spider-Man's transition manager (requests can be made)
	};

	// ---- guest frame capture @0x800 (seqlock) --------------------------------------------------
	// Spider-Man's rendered frame, cut down to what is near his camera (the hero and his webs: in sky
	// mode New York is a kilometer below), in D3D12 textures shared by name. Each texel is
	// (r, g, b, distance in meters), distance 0 = nothing there. The host draws them over its view.
	enum CaptureFlags : std::uint32_t
	{
		kCapLive = 1u << 0,  // frames are coming; `slot` holds the newest finished one
	};

	inline constexpr std::uint32_t kCaptureSlots = 4;
	// swprintf(name, kCaptureNameFmt, guestPid, generation, slot)
	inline constexpr wchar_t kCaptureNameFmt[] = L"Local\\ArkWebCap_%u_%u_%u";

	struct CaptureState
	{
		std::uint32_t seq;
		std::uint32_t flags;       // CaptureFlags
		std::uint32_t generation;  // bumps whenever the textures are recreated (size change)
		std::uint32_t slot;        // texture holding the newest finished frame
		std::uint64_t frameId;     // frames finished so far
		std::int64_t  qpc;         // when Spider-Man presented that frame (QueryPerformanceCounter)
		std::uint32_t width;       // texture size = Spider-Man's back buffer
		std::uint32_t height;
		float         tanHalfFovX; // Spider-Man's projection for that frame; 0 = not known
		float         tanHalfFovY;
		double        camPos[3];   // the camera of that frame, guest space (anchored like GuestState)
		float         camRot[9];   // rows: side, up, forward
		float         range;       // pixels nearer than this (meters) were kept
		std::uint32_t format;      // DXGI_FORMAT of the textures
		std::uint32_t share;       // CaptureShare: how the host opens the textures
		std::uint64_t handles[kCaptureSlots];  // kShareLegacy: global D3D11 shared handles (ID3D11Device::OpenSharedResource)
		std::uint8_t  reserved[0x200 - 0x98];
	};
	static_assert(sizeof(CaptureState) == 0x200);

	enum CaptureShare : std::uint32_t
	{
		kShareNamed = 0,   // D3D12 shared resources, opened by name (kCaptureNameFmt)
		kShareLegacy = 1,  // D3D11 (11on12) textures with legacy shared handles in `handles`
	};

	// ---- host -> guest: Batman's pose @0xA00 (seqlock) -----------------------------------------
	// Batman's skeleton bones named in kPoseBoneNames, in that order, as positions in Spider-Man's model space
	// (meters; +x his left, +y up, +z forward). The guest turns Spider-Man's matching joints the same way
	// ("mirror": he takes Batman's pose, Batman's combat animations included).
	inline constexpr std::uint64_t kOffPose = 0xA00;
	inline constexpr std::uint32_t kPoseBones = 25;
	inline constexpr const char*   kPoseBoneNames[kPoseBones] = {
		"Bip01_Pelvis", "Bip01_Spine", "Bip01_Spine1", "Bip01_Spine2", "Bip01_Spine3", "Bip01_Neck", "Bip01_Head",       // 0-6
		"Bip01_L_Clavicle", "Bip01_L_UpperArm", "Bip01_L_Forearm", "Bip01_L_Hand",                                       // 7-10
		"Bip01_R_Clavicle", "Bip01_R_UpperArm", "Bip01_R_Forearm", "Bip01_R_Hand",                                       // 11-14
		"Bip01_L_Thigh", "Bip01_L_Calf", "Bip01_L_Foot", "Bip01_R_Thigh", "Bip01_R_Calf", "Bip01_R_Foot",                // 15-20
		"Bip01_L_Toe0", "Bip01_R_Toe0", "Bip01_L_Finger2", "Bip01_R_Finger2",                                            // 21-24
	};

	enum PoseFlags : std::uint32_t
	{
		kPoseValid = 1u << 0,  // every named bone was found and `bone` holds this tick's pose
	};

	struct PoseState
	{
		std::uint32_t seq;
		std::uint32_t flags;          // PoseFlags
		std::uint64_t frame;          // host tick it was taken on
		std::uint32_t count;          // kPoseBones
		std::uint32_t reserved0;
		float         bone[kPoseBones][3];
		std::uint8_t  reserved[0x200 - 0x18 - kPoseBones * 12];
	};
	static_assert(sizeof(PoseState) == 0x200);

	// ---- byte rings (SPSC) ---------------------------------------------------------------------
	// [u64 head (total bytes written)] pad to 0x40 [u64 tail (total bytes consumed)] pad to 0x80,
	// data follows. A record is RingRecord + payload, 8-byte aligned; it never wraps: a writer
	// that would cross the end writes a kRecPad record (or just skips if < 8 bytes remain) and
	// continues at the start.
	inline constexpr std::uint64_t kRingHeadOff = 0x00;
	inline constexpr std::uint64_t kRingTailOff = 0x40;
	inline constexpr std::uint64_t kRingDataOff = 0x80;

	enum RecordType : std::uint32_t
	{
		kRecPad = 0,
		kRecCommand = 1,  // payload: one command line (ASCII, no terminator), as in logs\<game>_cmd.txt
		kRecTile = 2,     // payload: TileRecord, then awtBytes of AWT1, then awhBytes of AWH1
	};

	struct RingRecord
	{
		std::uint32_t type;   // RecordType
		std::uint32_t bytes;  // payload bytes (record occupies align8(8 + bytes))
	};
	static_assert(sizeof(RingRecord) == 8);

	struct TileRecord
	{
		char          key[32];   // tile name, NUL-terminated
		std::uint32_t awtBytes;  // collision (AWT1, absolute guest space)
		std::uint32_t awhBytes;  // swing hints (AWH1, absolute guest space), may be 0
	};
	static_assert(sizeof(TileRecord) == 40);

	inline constexpr std::uint64_t Align8(std::uint64_t a_n) { return (a_n + 7) & ~7ull; }
}
