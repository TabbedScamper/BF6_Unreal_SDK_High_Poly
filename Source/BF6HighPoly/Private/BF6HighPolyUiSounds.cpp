// ============================================================================
// The game's own UI sounds, decoded and handed to the tool.
//
// EVERY SOUND HERE IS THE GAME'S OWN CHOICE, not ours. The events the tool
// raises are the ones a menu raises, so each one is mapped onto the asset the
// game's own menu-navigation style already names for that job:
//
//   Place, Confirm    primaryactivation   the sound a menu makes when you commit
//   Select, MoveEnd   secondaryselect     its lighter twin, for a lesser commit
//   Hover             focus               what the game plays as focus moves
//   Delete, Cancel    goback              backing out
//   Error             actionfailed        the game's own name for a refusal
//   Undo, Redo        toggleoff / toggleon
//   Assign            captureobjectives / oncapturedbyfriendly
//   Link, Unlink      captureobjectives / objectiveonenter, objectiveonexit
//   Save, ImportDone  deploy / screen / actionsuccess
//   RingOpen/Close    the mod builder's own submenu open and close
//
// Two of those need a word. Error was going to be the countdown tick, and the
// install turns out to ship bf03_ui_menunavigation_default_ACTIONFAILED, which
// is the game saying the thing you just did did not happen. That is the sound,
// and picking the tick over it would have been us choosing by ear over the
// game's own label. And Link/Unlink borrow the objective enter and exit
// stingers because a link in this tool IS an object being attached to and
// detached from something, which is what those two sounds mean in a match.
//
// Every event carries a FALLBACK from the shared mount, so the tool has a voice
// seconds after the editor opens rather than eighty five seconds later.
// ============================================================================

#include "BF6HighPolyUiSounds.h"
#include "BF6HighPolyCore.h"
#include "BF6HighPolyDiskCache.h"
#include "BF6HighPolyPreviews.h"
#include "BF6HighPolyShared.h"
#include "BF6SDKExtension.h"

#include "Async/Async.h"
#include "Editor.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Sound/SoundWaveProcedural.h"
#include "UObject/UObjectGlobals.h"

// This module keeps its own log category, like every other file in the
// add-on: one grep finds every line the UI sounds wrote.
DEFINE_LOG_CATEGORY_STATIC(LogBF6HPUiSound, Log, All);

struct bf6_ctx;

namespace
{
	FCriticalSection GSoundJobsMutex;
	TArray<TFuture<void>> GSoundJobs;

	// Check priority while holding the reader lock: checking first and then
	// waiting for the lock lets a sound start in the middle of a level build.
	bool LockSoundReader()
	{
		while (!BF6HP::Shared::CoreShuttingDown())
		{
			if (BF6HP::Shared::CoreMutex().TryLock())
			{
				if (!BF6HP::Shared::GeometryHasPriority()) return true;
				BF6HP::Shared::CoreMutex().Unlock();
			}
			FPlatformProcess::Sleep(0.05f);
		}
		return false;
	}
	// ---- the C ABI this module needs ---------------------------------------
	//
	// Resolved by name off the add-on's own dll handle, so an older
	// bf6_core.dll simply leaves these null and the add-on stays silent instead
	// of crashing. Feature detection, the same way the rest of the core is
	// reached.
	typedef int (*FnMountAll)(bf6_ctx*, int, char*, int);
	typedef int (*FnDecode)(bf6_ctx*, const char*, int, int16*, int, int*, int*);

	struct FSfxRow
	{
		const char* Placeable;
		const char* Blueprint;
		const char* SoundEbx;
		int32       VariationCount;
		int32       Loop;
		int32       TwoD;
		int32       Channels;
		int32       SampleRate;
		float       DurationSec;
		const char* Status;
	};
	typedef int (*FnSfxResolve)(bf6_ctx*, const char*, FSfxRow*, int);

	FnMountAll   GMountAll   = nullptr;
	FnDecode     GDecode     = nullptr;
	FnSfxResolve GSfxResolve = nullptr;
	bool         GSymbolsTried = false;

	BF6HP::FCore* GCore = nullptr;

	bool ResolveSymbols()
	{
		if (GSymbolsTried) return GDecode != nullptr;
		if (!GCore || !GCore->DllHandle()) return false;
		GSymbolsTried = true;
		void* Dll = GCore->DllHandle();
		GMountAll   = (FnMountAll)  FPlatformProcess::GetDllExport(Dll, TEXT("bf6_mount_all"));
		GDecode     = (FnDecode)    FPlatformProcess::GetDllExport(Dll, TEXT("bf6_ui_sound_decode"));
		GSfxResolve = (FnSfxResolve)FPlatformProcess::GetDllExport(Dll, TEXT("bf6_sfx_placeable_resolve"));
		if (!GDecode)
			UE_LOG(LogBF6HPUiSound, Warning,
				TEXT("ui sounds: this bf6_core.dll has no bf6_ui_sound_decode, the tool stays silent"));
		return GDecode != nullptr;
	}

	// ---- the mapping --------------------------------------------------------

	struct FEventMap
	{
		BF6Ext::EUiSound Event;
		const TCHAR*     Preferred;   // Portal-authored, needs the catalogue mount
		const TCHAR*     Fallback;    // shared mount, available at once
	};

	const TCHAR* const kPortalNav =
		TEXT("common/sound/portal/ui/menunavigation/default/bf03_portal_ui_menunavigation_default_");
	const TCHAR* const kBaseNav =
		TEXT("common/sound/ui/menunavigation/default/bf03_ui_menunavigation_default_");
	const TCHAR* const kObjective =
		TEXT("common/sound/portal/ui/gamemode/shared/captureobjectives/bf03_portal_ui_gamemode_shared_captureobjectives_");
	const TCHAR* const kDeploy =
		TEXT("common/sound/portal/ui/deploy/screen/bf03_portal_ui_deploy_screen_");
	const TCHAR* const kModBuilder =
		TEXT("game/glacierportal/modbuilder/audio/bf03_portal_ui_");

	FString Portal(const TCHAR* Leaf) { return FString(kPortalNav) + Leaf + TEXT("-oneshot2d_config_01"); }
	FString Base(const TCHAR* Leaf)   { return FString(kBaseNav) + Leaf + TEXT("_config_01"); }
	FString Objective(const TCHAR* L) { return FString(kObjective) + L + TEXT("-oneshot2d_config_01"); }
	FString Deploy(const TCHAR* L)    { return FString(kDeploy) + L + TEXT("-oneshot2d_config_01"); }
	FString ModBuilder(const TCHAR* L){ return FString(kModBuilder) + L + TEXT("_config_01"); }

	struct FEventChoice
	{
		FString Preferred;
		FString Fallback;
	};

	void BuildChoices(TArray<FEventChoice>& Out)
	{
		Out.SetNum(int32(BF6Ext::EUiSound::Count));
		auto Set = [&Out](BF6Ext::EUiSound E, const FString& P, const FString& F)
		{
			Out[int32(E)].Preferred = P;
			Out[int32(E)].Fallback  = F;
		};
		using E = BF6Ext::EUiSound;
		Set(E::Place,      Portal(TEXT("primaryactivation")), Base(TEXT("primaryactivation")));
		Set(E::Confirm,    Portal(TEXT("primaryactivation")), Base(TEXT("primaryactivation")));
		Set(E::Select,     Portal(TEXT("secondaryselect")),   Base(TEXT("secondaryactivation")));
		Set(E::MoveEnd,    Portal(TEXT("secondaryselect")),   Base(TEXT("slidersclickdown")));
		Set(E::Hover,      Portal(TEXT("focus")),             Base(TEXT("focus")));
		Set(E::Delete,     Portal(TEXT("goback")),            Base(TEXT("goback")));
		Set(E::Cancel,     Portal(TEXT("goback")),            Base(TEXT("goback")));
		Set(E::Error,      Base(TEXT("actionfailed")),        Base(TEXT("actionfailed")));
		Set(E::Undo,       Portal(TEXT("toggleoff")),         Base(TEXT("turnoff")));
		Set(E::Redo,       Portal(TEXT("toggleon")),          Base(TEXT("turnon")));
		Set(E::Assign,     Objective(TEXT("oncapturedbyfriendly")), Base(TEXT("turnon")));
		Set(E::Link,       Objective(TEXT("objectiveonenter")),     Base(TEXT("turnon")));
		Set(E::Unlink,     Objective(TEXT("objectiveonexit")),      Base(TEXT("turnoff")));
		Set(E::Save,       Deploy(TEXT("actionsuccess")),     Base(TEXT("turnon")));
		Set(E::ImportDone, Deploy(TEXT("actionsuccess")),     Base(TEXT("turnon")));
		Set(E::RingOpen,   ModBuilder(TEXT("submenu_open_2d")),  Base(TEXT("secondaryactivation")));
		Set(E::RingClose,  ModBuilder(TEXT("submenu_close_2d")), Base(TEXT("goback")));
	}

	// ---- decoded PCM --------------------------------------------------------

	struct FClip
	{
		TArray<uint8> Pcm;      // interleaved 16-bit, little endian
		int32 Channels = 0;
		int32 Rate = 0;
		FString Ebx;
		bool IsValid() const { return Pcm.Num() > 0 && Channels > 0 && Rate > 0; }
	};

	// The cache version. Bump it when what the bytes MEAN changes, or a stale
	// blob loads cleanly and lies.
	constexpr uint32 kClipCacheVersion = 1;

	FString CacheNameFor(const FString& Ebx)
	{
		// The ebx path has slashes in it, so it cannot be a file name. Its hash
		// can, and the path is stored in the blob so a collision is caught.
		return FString::Printf(TEXT("uisound_%08x"), GetTypeHash(Ebx));
	}

	bool LoadClipFromCache(const FString& Ebx, FClip& Out)
	{
		TArray<uint8> Raw;
		if (!BF6HP::DiskCache::LoadPacked(TEXT("shared"), CacheNameFor(Ebx), kClipCacheVersion, Raw))
			return false;
		// [pathLen][path][channels][rate][pcm]
		if (Raw.Num() < 12) return false;
		int32 At = 0;
		auto ReadInt = [&Raw, &At](int32& V) -> bool
		{
			if (At + 4 > Raw.Num()) return false;
			FMemory::Memcpy(&V, Raw.GetData() + At, 4);
			At += 4;
			return true;
		};
		int32 PathLen = 0, Channels = 0, Rate = 0;
		if (!ReadInt(PathLen) || PathLen < 0 || At + PathLen > Raw.Num()) return false;
		const FString Stored = FString::ConstructFromPtrSize(
			reinterpret_cast<const ANSICHAR*>(Raw.GetData() + At), PathLen);
		At += PathLen;
		if (Stored != Ebx) return false;   // hash collision, or a renamed asset
		if (!ReadInt(Channels) || !ReadInt(Rate)) return false;
		if (Channels <= 0 || Rate <= 0) return false;
		Out.Channels = Channels;
		Out.Rate = Rate;
		Out.Ebx = Ebx;
		Out.Pcm.Append(Raw.GetData() + At, Raw.Num() - At);
		return Out.IsValid();
	}

	void SaveClipToCache(const FClip& Clip)
	{
		if (!Clip.IsValid()) return;
		const FTCHARToUTF8 Utf8(*Clip.Ebx);
		const int32 PathLen = Utf8.Length();
		TArray<uint8> Raw;
		Raw.Reserve(12 + PathLen + Clip.Pcm.Num());
		auto PutInt = [&Raw](int32 V)
		{
			const uint8* P = reinterpret_cast<const uint8*>(&V);
			Raw.Append(P, 4);
		};
		PutInt(PathLen);
		Raw.Append(reinterpret_cast<const uint8*>(Utf8.Get()), PathLen);
		PutInt(Clip.Channels);
		PutInt(Clip.Rate);
		Raw.Append(Clip.Pcm);
		BF6HP::DiskCache::SaveAsync(TEXT("shared"), CacheNameFor(Clip.Ebx),
			kClipCacheVersion, MoveTemp(Raw), true);
	}

	// Worker-thread decode: cache first, install second.
	bool DecodeClip(bf6_ctx* Ctx, const FString& Ebx, FClip& Out)
	{
		if (Ebx.IsEmpty()) return false;
		if (LoadClipFromCache(Ebx, Out)) return true;
		if (!Ctx || !GDecode) return false;
		// One decode, one lock. The context is shared with the placed resolver on
		// the game thread and with the preview mounts; each call is atomic and
		// short, which is what lets the editor keep drawing while this runs.
		if (!LockSoundReader()) return false;
		ON_SCOPE_EXIT { BF6HP::Shared::CoreMutex().Unlock(); };
		const FTCHARToUTF8 Path(*Ebx);
		int32 Channels = 0, Rate = 0;
		const int32 Need = GDecode(Ctx, Path.Get(), 0, nullptr, 0, &Channels, &Rate);
		if (Need <= 0 || Channels <= 0 || Rate <= 0) return false;
		TArray<int16> Samples;
		Samples.SetNumUninitialized(Need);
		const int32 Got = GDecode(Ctx, Path.Get(), 0, Samples.GetData(), Need, &Channels, &Rate);
		if (Got != Need) return false;
		Out.Channels = Channels;
		Out.Rate = Rate;
		Out.Ebx = Ebx;
		Out.Pcm.Append(reinterpret_cast<const uint8*>(Samples.GetData()), Got * int32(sizeof(int16)));
		SaveClipToCache(Out);
		return Out.IsValid();
	}

	// ---- the waves ----------------------------------------------------------
	//
	// USoundWaveProcedural, not a plain USoundWave. A transient USoundWave has
	// no cooked or compressed data, so the audio mixer has nothing to build a
	// decoder from and plays silence; a procedural wave is the engine's own
	// "I already have the PCM" path and needs no cooked bytes at all. Its queue
	// DRAINS as it plays, which is why the provider re-arms on every request
	// rather than handing back the same primed object twice.

	USoundWaveProcedural* MakeWave(const FClip& Clip, bool bLoop)
	{
		if (!Clip.IsValid()) return nullptr;
		USoundWaveProcedural* W = NewObject<USoundWaveProcedural>(
			GetTransientPackage(), NAME_None, RF_Transient | RF_Public);
		if (!W) return nullptr;
		W->SetSampleRate(Clip.Rate);
		W->NumChannels = Clip.Channels;
		W->Duration = float(Clip.Pcm.Num()) /
			float(Clip.Channels * 2 * Clip.Rate);
		W->SoundGroup = SOUNDGROUP_Default;
		W->bLooping = false;   // the loop is served by the underflow hook below
		W->AddToRoot();
		if (bLoop)
		{
			// A looping placeable preview keeps going until the user stops it.
			// The engine asks for more when the queue runs dry, which is the
			// designed way to do this and costs nothing while idle.
			TArray<uint8> Bytes = Clip.Pcm;
			W->OnSoundWaveProceduralUnderflow.BindLambda(
				[Bytes](USoundWaveProcedural* Proc, int32 /*SamplesNeeded*/)
				{
					if (Proc) Proc->QueueAudio(Bytes.GetData(), Bytes.Num());
				});
		}
		return W;
	}

	void ReleaseWave(USoundWaveProcedural*& W)
	{
		if (!W) return;
		if (UObjectInitialized())
		{
			W->OnSoundWaveProceduralUnderflow.Unbind();
			W->RemoveFromRoot();
		}
		W = nullptr;
	}

	// ---- the provider -------------------------------------------------------

	class FUiSoundProvider : public BF6Ext::IUiSoundProvider
	{
	public:
		virtual ~FUiSoundProvider() { ReleaseAll(); }

		// Game thread only: called from BF6UiSound::Play.
		virtual USoundWave* SoundFor(BF6Ext::EUiSound Event) override
		{
			const int32 i = int32(Event);
			if (!Waves.IsValidIndex(i) || !Waves[i]) return nullptr;
			// Re-arm: the queue from the previous play has drained.
			Waves[i]->ResetAudio();
			Waves[i]->QueueAudio(Clips[i].Pcm.GetData(), Clips[i].Pcm.Num());
			return Waves[i];
		}

		virtual FString Describe() const override { return Summary; }

		// Called on the game thread once the worker has clips.
		void Adopt(TArray<FClip>&& InClips, const FString& InSummary)
		{
			ReleaseAll();
			Clips = MoveTemp(InClips);
			Waves.SetNumZeroed(int32(BF6Ext::EUiSound::Count));
			for (int32 i = 0; i < Clips.Num() && i < Waves.Num(); ++i)
				Waves[i] = MakeWave(Clips[i], false);
			Summary = InSummary;
		}

		void ReleaseAll()
		{
			for (USoundWaveProcedural*& W : Waves) ReleaseWave(W);
			Waves.Reset();
			Clips.Reset();
		}

	private:
		TArray<FClip>                  Clips;
		TArray<USoundWaveProcedural*>  Waves;
		FString                        Summary;
	};

	// ---- the object library's SFX preview -----------------------------------

	class FSfxPreview : public BF6Ext::IPlaceableSoundPreview
	{
	public:
		virtual ~FSfxPreview() { StopInternal(); }

		virtual bool CanPreview(const FString& Type) const override
		{
			// Asked while a row is built, so this may not touch the install.
			// The name is the whole test: an SFX_* placeable is a sound, and
			// whether THIS one resolves is answered when it is played.
			return GDecode != nullptr && Type.StartsWith(TEXT("SFX_"));
		}

		virtual void Preview(const FString& Type) override
		{
			StopInternal();
			Current = Type;
			// Resolve and decode off the game thread; the catalogue mount alone
			// can take a minute the first time.
			const FString Wanted = Type;
			if (BF6HP::Shared::CoreShuttingDown()) return;
			FScopeLock JobsLock(&GSoundJobsMutex);
			GSoundJobs.RemoveAll([](const TFuture<void>& Job) { return Job.IsReady(); });
			GSoundJobs.Add(Async(EAsyncExecution::ThreadPool, [this, Wanted]
			{
				FString Error;
				if (!BF6HP::PreviewsEnsureCore(Error) ||
					!BF6HP::PreviewsMountCatalogue(Error))
				{
					UE_LOG(LogBF6HPUiSound, Warning,
						TEXT("sfx preview: %s"), *Error);
					return;
				}
				BF6HPUiSounds::OnCatalogueMounted();
				bf6_ctx* Ctx = GCore ? GCore->Handle() : nullptr;
				if (!Ctx || !GSfxResolve) return;
				const FTCHARToUTF8 TypeUtf8(*Wanted);
				FSfxRow Rows[4]{};
				int32 N = 0;
				{
					if (!LockSoundReader()) return;
					ON_SCOPE_EXIT { BF6HP::Shared::CoreMutex().Unlock(); };
					N = GSfxResolve(Ctx, TypeUtf8.Get(), Rows, 4);
				}
				if (N <= 0)
				{
					UE_LOG(LogBF6HPUiSound, Log,
						TEXT("sfx preview: %s has no sound in the install (%s)"),
						*Wanted, Rows[0].Status ? ANSI_TO_TCHAR(Rows[0].Status) : TEXT("no row"));
					return;
				}
				const FString Ebx = ANSI_TO_TCHAR(Rows[0].SoundEbx);
				const bool bLoop = Rows[0].Loop != 0;
				FClip Clip;
				if (!DecodeClip(Ctx, Ebx, Clip))
				{
					UE_LOG(LogBF6HPUiSound, Warning,
						TEXT("sfx preview: %s resolved to %s but did not decode"),
						*Wanted, *Ebx);
					return;
				}
				AsyncTask(ENamedThreads::GameThread,
					[this, Wanted, Ebx, bLoop, Clip = MoveTemp(Clip)]() mutable
					{
						// The user may have moved on while that ran.
						if (BF6HP::Shared::CoreShuttingDown()) return;
						if (Current != Wanted) return;
						StopInternal();
						Current = Wanted;
						Clip_ = MoveTemp(Clip);
						Wave = MakeWave(Clip_, bLoop);
						if (!Wave || !GEditor) { Current.Reset(); return; }
						Wave->QueueAudio(Clip_.Pcm.GetData(), Clip_.Pcm.Num());
						Wave->Volume = BF6Ext::UiSoundVolume();
						GEditor->PlayPreviewSound(Wave);
						UE_LOG(LogBF6HPUiSound, Log,
							TEXT("sfx preview: %s -> %s (%s, %.2fs)"),
							*Wanted, *Ebx, bLoop ? TEXT("loop") : TEXT("one shot"),
							Clip_.Rate ? float(Clip_.Pcm.Num()) /
								float(Clip_.Channels * 2 * Clip_.Rate) : 0.f);
					});
			}));
		}

		virtual void Stop() override { StopInternal(); }

		virtual FString Playing() const override { return Current; }

	private:
		void StopInternal()
		{
			if (GEditor && Wave) GEditor->ResetPreviewAudioComponent();
			ReleaseWave(Wave);
			Clip_ = FClip();
			Current.Reset();
		}

		FString                Current;
		FClip                  Clip_;
		USoundWaveProcedural*  Wave = nullptr;
	};

	TSharedPtr<FUiSoundProvider> GProvider;
	TSharedPtr<FSfxPreview>      GPreview;
	FThreadSafeBool              GResolving(false);
	bool                         GHaveCatalogue = false;

	// The worker: resolve every event to an asset and decode it. Preferred
	// first, fallback second, and the log says which one each event landed on
	// so a wrong sound can be traced to a path rather than to an opinion.
	void ResolveOnWorker()
	{
		FScopeLock JobsLock(&GSoundJobsMutex);
		if (BF6HP::Shared::CoreShuttingDown()) return;
		if (GResolving) return;
		GResolving = true;
		GSoundJobs.RemoveAll([](const TFuture<void>& Job) { return Job.IsReady(); });
		GSoundJobs.Add(Async(EAsyncExecution::ThreadPool, []
		{
			ON_SCOPE_EXIT{ GResolving = false; };
			FString Error;
			if (!BF6HP::PreviewsEnsureCore(Error))
			{
				UE_LOG(LogBF6HPUiSound, Log, TEXT("ui sounds: %s"), *Error);
				return;
			}
			if (!ResolveSymbols()) return;
			bf6_ctx* Ctx = GCore ? GCore->Handle() : nullptr;
			if (!Ctx) return;

			// LET THE MAP FINISH APPEARING FIRST.
			//
			// Everything below reads the shared context: the mount just under
			// here is bf6_mount_all, the whole install, and each clip decode is
			// about a second inside the reader. Run against a level open that
			// cost 17.6 seconds of reader time, during which the placed resolver
			// was refused 556 times and threw away its merge each time. These
			// are the sounds the tool's own buttons make; they are worth nothing
			// against the scene the creator is waiting for, so they wait.
			//
			// ASKED AGAIN BEFORE EVERY CALL, not once at the top. This job
			// starts with the editor and a map opens after it, so a single check
			// here is answered "nothing is waiting" and then the whole pass runs
			// straight through the level open anyway - which is exactly what was
			// measured the first time this gate was tried.
			//
			// A long build keeps priority until it completes or shuts down.
			// A timeout must not let audio re-enter a reader that is still busy.
			auto StandOffForGeometry = []()
			{
				while (BF6HP::Shared::GeometryHasPriority()
					&& !BF6HP::Shared::CoreShuttingDown())
				{
					FPlatformProcess::Sleep(0.25f);
				}
				return !BF6HP::Shared::CoreShuttingDown();
			};
			if (!StandOffForGeometry()) return;

			// The native calls below each take the one shared context lock, inside
			// DecodeClip and around the mount. It is taken PER CALL rather than for
			// the whole pass: this is a background job, the game thread wants the
			// same lock to resolve placed objects, and holding it for seventeen
			// decodes made the editor hitch for a third of a second at a time.			// The shared mount is what the cheap pass reads. It is idempotent
			// in the core, and it never narrows a wider mount someone else
			// already made.
			if (GMountAll)
			{
				if (!LockSoundReader()) return;
				ON_SCOPE_EXIT { BF6HP::Shared::CoreMutex().Unlock(); };
				char Err[512]{};
				GMountAll(Ctx, 0, Err, int(sizeof(Err)));
			}

			TArray<FEventChoice> Choices;
			BuildChoices(Choices);
			TArray<FClip> Clips;
			Clips.SetNum(Choices.Num());
			int32 Decoded = 0;
			TArray<FString> Missing;
			for (int32 i = 0; i < Choices.Num(); ++i)
			{
				// A map can open at any point in this loop, and sixteen clips at
				// about a second each is sixteen seconds of reader the scene
				// wants more than this does.
				if (!StandOffForGeometry()) return;
				const FEventChoice& C = Choices[i];
				if (!DecodeClip(Ctx, C.Preferred, Clips[i]))
					DecodeClip(Ctx, C.Fallback, Clips[i]);
				if (Clips[i].IsValid())
				{
					Decoded++;
					UE_LOG(LogBF6HPUiSound, Log, TEXT("ui sound: event %d -> %s"),
						i, *Clips[i].Ebx);
				}
				else
				{
					Missing.Add(FString::Printf(TEXT("%d"), i));
				}
			}
			const FString Summary = FString::Printf(
				TEXT("%d event(s) mapped, %d decoded%s%s"),
				Choices.Num(), Decoded,
				Missing.Num() ? TEXT(", missing: ") : TEXT(""),
				Missing.Num() ? *FString::Join(Missing, TEXT(" ")) : TEXT(""));
			UE_LOG(LogBF6HPUiSound, Log, TEXT("ui sounds: %s"), *Summary);

			AsyncTask(ENamedThreads::GameThread,
				[Clips = MoveTemp(Clips), Summary]() mutable
				{
					if (BF6HP::Shared::CoreShuttingDown()) return;
					if (!GProvider.IsValid()) GProvider = MakeShared<FUiSoundProvider>();
					GProvider->Adopt(MoveTemp(Clips), Summary);
					BF6Ext::RegisterUiSoundProvider(GProvider);
				});
		}));
	}
}

namespace BF6HPUiSounds
{
	void Attach(BF6HP::FCore* Core)
	{
		if (!Core) return;
		GCore = Core;
		if (!GPreview.IsValid())
		{
			GPreview = MakeShared<FSfxPreview>();
			BF6Ext::RegisterPlaceableSoundPreview(GPreview);
		}
		// Nothing is decoded when the user has the tool's sounds turned off;
		// they turn them back on and the next attach or catalogue mount does
		// the work.
		if (BF6Ext::UiSoundEnabled()) ResolveOnWorker();
	}

	void Detach()
	{
		// Workers use the native context and callbacks refer to preview state.
		// Drain them before releasing either; queued callbacks check shutdown.
		TArray<TFuture<void>> Pending;
		{
			FScopeLock JobsLock(&GSoundJobsMutex);
			Pending = MoveTemp(GSoundJobs);
		}
		for (TFuture<void>& Job : Pending) Job.Wait();
		BF6Ext::RegisterUiSoundProvider(nullptr);
		BF6Ext::RegisterPlaceableSoundPreview(nullptr);
		if (GPreview.IsValid()) GPreview->Stop();
		GPreview.Reset();
		if (GProvider.IsValid()) GProvider->ReleaseAll();
		GProvider.Reset();
		GCore = nullptr;
	}

	void OnCatalogueMounted()
	{
		if (GHaveCatalogue) return;
		GHaveCatalogue = true;
		// The Portal-authored sounds are reachable now, so re-resolve and let
		// the preferred asset win where there is one.
		ResolveOnWorker();
	}
}
