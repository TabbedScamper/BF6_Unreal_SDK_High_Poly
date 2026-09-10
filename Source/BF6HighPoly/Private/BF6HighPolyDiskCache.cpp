#include "BF6HighPolyDiskCache.h"

#include "Async/Async.h"
#include "Compression/OodleDataCompression.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include <atomic>

DEFINE_LOG_CATEGORY_STATIC(LogBF6DiskCache, Log, All);

namespace BF6HP::DiskCache
{
	namespace
	{
		FString GInstallDir;
		FString GRoot;
		FString GSignature;
		FStats  GStats;
		FCriticalSection GStatsLock;
		FCriticalSection GSignatureLock;
		FCriticalSection GPendingLock;
		TArray<TFuture<void>> GPending;
		std::atomic<int64> GPendingBytes{0};

		constexpr uint32 kMagic = 0x43364642; // 'BF6C' little-endian
		constexpr uint32 kCodecRaw = 0, kCodecOodle = 1;
		constexpr int32  kPackedHeader = 4 + 8;

		// One line per file: relative path, size, mtime. Order-stable because
		// FindFilesRecursive walks the tree the same way every time, and sorted
		// anyway so a different enumeration order cannot change the hash.
		FString DescribeInstall(const FString& Dir)
		{
			IFileManager& FM = IFileManager::Get();
			TArray<FString> Lines;
			for (const TCHAR* Sub : { TEXT("Data"), TEXT("Patch"), TEXT("Update") })
			{
				const FString Base = FPaths::Combine(Dir, Sub);
				if (!FM.DirectoryExists(*Base)) continue;
				TArray<FString> Files;
				FM.FindFilesRecursive(Files, *Base, TEXT("*.toc"), true, false, false);
				for (const FString& F : Files)
				{
					FString Rel = F;
					FPaths::MakePathRelativeTo(Rel, *(Dir + TEXT("/")));
					Lines.Add(FString::Printf(TEXT("%s|%lld|%lld"), *Rel,
						(long long)FM.FileSize(*F),
						(long long)FM.GetTimeStamp(*F).GetTicks()));
				}
			}
			const FString Exe = FPaths::Combine(Dir, TEXT("bf6.exe"));
			if (FM.FileExists(*Exe))
			{
				Lines.Add(FString::Printf(TEXT("bf6.exe|%lld|%lld"),
					(long long)FM.FileSize(*Exe),
					(long long)FM.GetTimeStamp(*Exe).GetTicks()));
			}
			Lines.Sort();
			return FString::Join(Lines, TEXT("\n"));
		}

		// The file write proper. Path is resolved by the caller so a worker
		// never reads the configuration globals.
		bool WriteFile(const FString& Path, uint32 Version, const TArray<uint8>& Bytes)
		{
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			TArray<uint8> File;
			File.Reserve(16 + Bytes.Num());
			const uint32 Magic = kMagic;
			const uint64 Len = (uint64)Bytes.Num();
			File.Append((const uint8*)&Magic, 4);
			File.Append((const uint8*)&Version, 4);
			File.Append((const uint8*)&Len, 8);
			File.Append(Bytes);
			// Write beside, then move: a crash mid-write must not leave a short
			// file that passes the length check by accident on the next open.
			// The temp name carries the thread so two writers of one blob (a
			// hit race between batches) cannot truncate each other's part.
			const FString Tmp = FString::Printf(TEXT("%s.%u.part"), *Path,
				FPlatformTLS::GetCurrentThreadId());
			if (!FFileHelper::SaveArrayToFile(File, *Tmp)) return false;
			IFileManager::Get().Delete(*Path, false, true, true);
			if (!IFileManager::Get().Move(*Path, *Tmp, true, true, true, true))
			{
				IFileManager::Get().Delete(*Tmp, false, true, true);
				return false;
			}
			FScopeLock G(&GStatsLock);
			GStats.Writes++;
			GStats.BytesWritten += Bytes.Num();
			return true;
		}
	}

	void Configure(const FString& InstallDir, const FString& CacheRoot)
	{
		FScopeLock G(&GSignatureLock);
		if (InstallDir != GInstallDir) GSignature.Reset();
		GInstallDir = InstallDir;
		GRoot = FPaths::Combine(CacheRoot, TEXT("derived"));
	}

	const FString& InstallSignature()
	{
		FScopeLock G(&GSignatureLock);
		if (GSignature.IsEmpty() && !GInstallDir.IsEmpty())
		{
			const double T0 = FPlatformTime::Seconds();
			const FString Desc = DescribeInstall(GInstallDir);
			GSignature = FMD5::HashAnsiString(*Desc).Left(16);
			int32 LineCount = Desc.IsEmpty() ? 0 : 1;
			for (const TCHAR Ch : Desc) if (Ch == TEXT('\n')) ++LineCount;
			UE_LOG(LogBF6DiskCache, Log,
				TEXT("install signature %s over %d catalogue line(s) in %.2fs"),
				*GSignature, LineCount, FPlatformTime::Seconds() - T0);
		}
		return GSignature;
	}

	FString PathFor(const FString& Level, const FString& Name)
	{
		const FString Sig = InstallSignature();
		FScopeLock G(&GSignatureLock);
		return FPaths::Combine(GRoot, Sig, Level.ToLower(), Name + TEXT(".bin"));
	}

	bool Load(const FString& Level, const FString& Name, uint32 Version, TArray<uint8>& Out)
	{
		Out.Reset();
		if (InstallSignature().IsEmpty()) return false;
		const double T0 = FPlatformTime::Seconds();
		const FString Path = PathFor(Level, Name);
		if (Path.IsEmpty()) return false;
		TArray<uint8> File;
		if (!FFileHelper::LoadFileToArray(File, *Path, FILEREAD_Silent) ||
			File.Num() < (int32)(sizeof(uint32) * 2 + sizeof(uint64)))
		{
			FScopeLock G(&GStatsLock);
			GStats.Misses++;
			return false;
		}
		const uint8* P = File.GetData();
		uint32 Magic, Ver; uint64 Len;
		FMemory::Memcpy(&Magic, P, 4); FMemory::Memcpy(&Ver, P + 4, 4);
		FMemory::Memcpy(&Len, P + 8, 8);
		if (Magic != kMagic || Ver != Version || (uint64)File.Num() != 16 + Len)
		{
			FScopeLock G(&GStatsLock);
			GStats.Misses++;
			return false;
		}
		Out.SetNumUninitialized((int32)Len);
		FMemory::Memcpy(Out.GetData(), P + 16, Len);
		FScopeLock G(&GStatsLock);
		GStats.Hits++;
		GStats.BytesRead += (int64)Len;
		GStats.SecondsLoading += FPlatformTime::Seconds() - T0;
		return true;
	}

	bool Save(const FString& Level, const FString& Name, uint32 Version, const TArray<uint8>& Bytes)
	{
		if (InstallSignature().IsEmpty()) return false;
		const FString Path = PathFor(Level, Name);
		if (Path.IsEmpty()) return false;
		const double T0 = FPlatformTime::Seconds();
		const bool bOk = WriteFile(Path, Version, Bytes);
		FScopeLock G(&GStatsLock);
		GStats.SecondsSaving += FPlatformTime::Seconds() - T0;
		return bOk;
	}

	void Pack(const TArray<uint8>& Raw, TArray<uint8>& OutPacked)
	{
		OutPacked.Reset();
		uint32 Codec = kCodecRaw;
		const uint64 RawLen = (uint64)Raw.Num();
		TArray<uint8> Compressed;
		// Kraken at SuperFast: a few hundred MB/s to encode on one core, well
		// over a GB/s to decode, which is the side that runs on every warm open.
		if (Raw.Num() >= 4096)
		{
			const int64 Need = FOodleDataCompression::CompressedBufferSizeNeeded(Raw.Num());
			Compressed.SetNumUninitialized((int32)Need);
			const int64 Got = FOodleDataCompression::Compress(
				Compressed.GetData(), Need, Raw.GetData(), Raw.Num(),
				FOodleDataCompression::ECompressor::Kraken,
				FOodleDataCompression::ECompressionLevel::SuperFast);
			if (Got > 0 && Got <= (int64)(Raw.Num() * 0.94))
			{
				Compressed.SetNum((int32)Got, EAllowShrinking::No);
				Codec = kCodecOodle;
			}
		}
		const TArray<uint8>& Body = Codec == kCodecOodle ? Compressed : Raw;
		OutPacked.Reserve(kPackedHeader + Body.Num());
		OutPacked.Append((const uint8*)&Codec, 4);
		OutPacked.Append((const uint8*)&RawLen, 8);
		OutPacked.Append(Body);
		FScopeLock G(&GStatsLock);
		GStats.BytesRawPacked += Raw.Num();
	}

	bool Unpack(const TArray<uint8>& Packed, TArray<uint8>& OutRaw)
	{
		OutRaw.Reset();
		if (Packed.Num() < kPackedHeader) return false;
		uint32 Codec; uint64 RawLen;
		FMemory::Memcpy(&Codec, Packed.GetData(), 4);
		FMemory::Memcpy(&RawLen, Packed.GetData() + 4, 8);
		if (RawLen > (uint64)MAX_int32) return false;
		const uint8* Body = Packed.GetData() + kPackedHeader;
		const int64 BodyLen = Packed.Num() - kPackedHeader;
		if (Codec == kCodecRaw)
		{
			if ((uint64)BodyLen != RawLen) return false;
			OutRaw.SetNumUninitialized((int32)RawLen);
			FMemory::Memcpy(OutRaw.GetData(), Body, RawLen);
			return true;
		}
		if (Codec != kCodecOodle) return false;
		OutRaw.SetNumUninitialized((int32)RawLen);
		if (!FOodleDataCompression::Decompress(OutRaw.GetData(), (int64)RawLen, Body, BodyLen))
		{
			OutRaw.Reset();
			return false;
		}
		return true;
	}

	bool LoadPacked(const FString& Level, const FString& Name, uint32 Version, TArray<uint8>& OutRaw)
	{
		TArray<uint8> Packed;
		if (!Load(Level, Name, Version, Packed)) return false;
		const double T0 = FPlatformTime::Seconds();
		const bool bOk = Unpack(Packed, OutRaw);
		FScopeLock G(&GStatsLock);
		GStats.SecondsLoading += FPlatformTime::Seconds() - T0;
		// A blob that will not unpack is a miss, not a hit that lied.
		if (!bOk) { GStats.Hits--; GStats.Misses++; }
		return bOk;
	}

	bool SavePacked(const FString& Level, const FString& Name, uint32 Version, const TArray<uint8>& Raw)
	{
		TArray<uint8> Packed;
		Pack(Raw, Packed);
		return Save(Level, Name, Version, Packed);
	}

	void SaveAsync(const FString& Level, const FString& Name, uint32 Version, TArray<uint8>&& Bytes, bool bPack)
	{
		if (InstallSignature().IsEmpty()) return;
		const FString Path = PathFor(Level, Name);
		if (Path.IsEmpty()) return;
		// Derived writes are optional. Do not retain an unbounded queue of raw
		// meshes/textures when the build outruns compression on a six-core CPU.
		// Skipping a write means a later cache miss, never a missing scene asset.
		constexpr int64 Budget = 128ll * 1024 * 1024;
		const int64 Size = Bytes.Num();
		int64 Pending = GPendingBytes.load();
		do
		{
			if (Size > Budget || Pending > Budget - Size)
			{
				FScopeLock G(&GStatsLock); ++GStats.WritesSkipped;
				return;
			}
		} while (!GPendingBytes.compare_exchange_weak(Pending, Pending + Size));
		TArray<uint8> Owned = MoveTemp(Bytes);
		TFuture<void> F = Async(EAsyncExecution::ThreadPool,
			[Path, Version, bPack, Size, Owned = MoveTemp(Owned)]() mutable
			{
				const double T0 = FPlatformTime::Seconds();
				if (bPack)
				{
					TArray<uint8> Packed;
					Pack(Owned, Packed);
					WriteFile(Path, Version, Packed);
				}
				else
				{
					WriteFile(Path, Version, Owned);
				}
				Owned.Empty();
				GPendingBytes -= Size;
				FScopeLock G(&GStatsLock);
				GStats.SecondsSaving += FPlatformTime::Seconds() - T0;
			});
		FScopeLock G(&GPendingLock);
		GPending.Add(MoveTemp(F));
		// Reap the finished ones so a long build does not hold thousands of
		// spent futures.
		if (GPending.Num() > 512)
			GPending.RemoveAll([](const TFuture<void>& P) { return P.IsReady(); });
	}

	void FlushPendingWrites()
	{
		TArray<TFuture<void>> Waiting;
		{
			FScopeLock G(&GPendingLock);
			Waiting = MoveTemp(GPending);
			GPending.Reset();
		}
		for (TFuture<void>& F : Waiting) F.Wait();
	}

	int32 Clear()
	{
		FlushPendingWrites();
		FString Root;
		{
			FScopeLock G(&GSignatureLock);
			Root = GRoot;
		}
		if (Root.IsEmpty()) return 0;
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("*.bin"), true, false, false);
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		return Files.Num();
	}

	FStats Stats() { FScopeLock G(&GStatsLock); return GStats; }
	void ResetStats() { FScopeLock G(&GStatsLock); GStats = FStats(); }
}
