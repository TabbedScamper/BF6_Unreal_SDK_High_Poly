// BF6HighPolyDiskCache - a memo of what the core already read from the game.
//
// WHY THIS EXISTS. A cold open of a map spends about 24 of its 78 seconds
// re-deriving the same four ground rasters from the same install: the far-
// field bake, the far ring, the per-pixel coverage and its 76 material
// sheets, plus the 16385^2 heightfield. Every one of those is a pure function
// of (this install, this level, these parameters). The game is still the only
// source of truth - nothing here is exported data handed to the add-on - it is
// a cache of the core's own answers, keyed so that a game patch or a change
// to what we ask for invalidates it on its own.
//
// KEY = install signature / level / name. The signature hashes the relative
// path, size and mtime of every .toc the install carries, plus bf6.exe, so a
// patch changes it. Each blob also carries a VERSION owned by the caller:
// bump it when what the bytes MEAN changes (a new field, a different layout)
// or a stale blob will load cleanly and lie. See the staleness ladder.
//
// THREADS. Load, Save, Pack and Unpack may be called from any thread at once;
// the object-phase prefetch loads a batch of texture and mesh blobs inside a
// ParallelFor. The stats are locked, the signature is computed under a lock,
// and every path is a pure function of its arguments.
#pragma once

#include "CoreMinimal.h"

namespace BF6HP::DiskCache
{
	// Where blobs live (a directory the add-on owns) and which install keys them.
	void Configure(const FString& InstallDir, const FString& CacheRoot);

	// Hex signature of the configured install. Computed once per session.
	const FString& InstallSignature();

	// Root/<signature>/<level>/<name>.bin - Level may be "shared" for blobs
	// that belong to a resource rather than a map (the material sheets).
	FString PathFor(const FString& Level, const FString& Name);

	// A blob is [magic][version][length][bytes]; a version mismatch is a miss.
	bool Load(const FString& Level, const FString& Name, uint32 Version, TArray<uint8>& Out);
	bool Save(const FString& Level, const FString& Name, uint32 Version, const TArray<uint8>& Bytes);

	// PACKED BLOBS. The payload is [codec][raw length][bytes], where codec 0 is
	// raw and 1 is Oodle Kraken. Pack compresses when that saves at least 6%
	// and stores raw otherwise - BCn texture blocks barely compress, mesh
	// floats and indices do - and Unpack does not need to be told which. A
	// raw-stored packed blob costs one memcpy over a plain one.
	void Pack(const TArray<uint8>& Raw, TArray<uint8>& OutPacked);
	bool Unpack(const TArray<uint8>& Packed, TArray<uint8>& OutRaw);
	bool LoadPacked(const FString& Level, const FString& Name, uint32 Version, TArray<uint8>& OutRaw);
	bool SavePacked(const FString& Level, const FString& Name, uint32 Version, const TArray<uint8>& Raw);

	// Write on a worker. The bytes are moved in; bPack runs Pack there too, so
	// the compress is off the calling thread as well as the disk write.
	// FlushPendingWrites waits for every outstanding write: the build calls it
	// before its cache summary so the numbers are final and no write outlives
	// the map that produced it.
	void SaveAsync(const FString& Level, const FString& Name, uint32 Version, TArray<uint8>&& Bytes, bool bPack);
	void FlushPendingWrites();

	// Remove everything under the root, every signature. Console-command bait.
	int32 Clear();

	struct FStats
	{
		int32  Hits = 0, Misses = 0, Writes = 0;
		int64  BytesRead = 0, BytesWritten = 0;
		// Bytes before Pack, for the blobs that went through it: the ratio
		// against BytesWritten is what the compression bought.
		int64  BytesRawPacked = 0;
		double SecondsLoading = 0.0;
		double SecondsSaving = 0.0;     // wall time on the writing threads
	};
	FStats Stats();          // a copy: the live one is written from workers
	void ResetStats();
}
