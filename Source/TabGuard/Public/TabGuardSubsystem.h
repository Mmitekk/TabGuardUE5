#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Containers/Ticker.h"
#include "TabGuardSubsystem.generated.h"

class IAssetEditorInstance;

/** Brief info about one slot's content (for menu labels). Plain C++ — not exposed to reflection. */
struct FTabGuardSlotInfo
{
	bool bExists = false;
	int32 AssetCount = 0;
	FDateTime SavedAtUtc;
};

/**
 * UTabGuardSubsystem — crash-safe snapshot of open asset tabs.
 * Saves on every asset open/close (debounced 0.5s) into Saved/TabGuard/TabSnapshot.json,
 * which the engine never touches, so it survives layout wipes and crashes.
 * Additionally supports 5 manual slots (Saved/TabGuard/Slots/Slot1..5.json)
 * for saving/loading named sets of tabs on demand.
 */
UCLASS()
class TABGUARD_API UTabGuardSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	/** Number of manual slots (SlotIndex is 0-based, UI shows 1-based). */
	static constexpr int32 MaxSlots = 5;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Write current open-asset list to autosave snapshot. Safe to call often (coalesced). */
	UFUNCTION(BlueprintCallable, Category = "TabGuard")
	void SaveSnapshot();

	/** Reopen every asset listed in the autosave snapshot. Skips already-open and missing assets. */
	UFUNCTION(BlueprintCallable, Category = "TabGuard")
	void RestoreSnapshot();

	/** Number of assets stored in autosave snapshot, or 0 if no snapshot yet. */
	UFUNCTION(BlueprintCallable, Category = "TabGuard")
	int32 GetSnapshotAssetCount() const;

	/** Save the currently open tabs into manual SlotIndex (0-based, valid range 0..MaxSlots-1). Overwrites. */
	UFUNCTION(BlueprintCallable, Category = "TabGuard")
	bool SaveSnapshotToSlot(int32 SlotIndex);

	/** Reopen tabs from manual SlotIndex (0-based). Returns false if the slot is empty/corrupted. */
	UFUNCTION(BlueprintCallable, Category = "TabGuard")
	bool RestoreSnapshotFromSlot(int32 SlotIndex);

	/** Number of assets stored in manual SlotIndex (0-based), or 0 if empty. */
	UFUNCTION(BlueprintCallable, Category = "TabGuard")
	int32 GetSlotAssetCount(int32 SlotIndex) const;

	static FString GetSnapshotFilePath();
	static FString GetBackupFilePath();
	static FString GetSlotFilePath(int32 SlotIndex);

	/** Reads slot file header (count + timestamp) without opening anything. */
	static bool GetSlotInfo(int32 SlotIndex, FTabGuardSlotInfo& OutInfo);

private:
	void BindAssetDelegates();
	void ScheduleSave();
	bool HandleDeferredSave(float DeltaTime);
	void OnAssetOpened(UObject* Asset, IAssetEditorInstance* Instance);
	void OnAssetClosed(UObject* Asset, IAssetEditorInstance* Instance);

	/** Collects currently open assets and writes them to FilePath. Returns false on IO/serialization failure. */
	bool WriteSnapshotToFile(const FString& FilePath);

	/** Reads a snapshot file and reopens listed assets. Returns restored count, or -1 if missing/corrupted. */
	int32 ReadSnapshotAndOpen(const FString& FilePath);

	FDelegateHandle PostEngineInitHandle;
	FDelegateHandle AssetOpenedHandle;
	FDelegateHandle AssetClosedHandle;
	FTSTicker::FDelegateHandle DeferredSaveHandle;
	bool bSavePending = false;
	bool bDelegatesBound = false;
};
