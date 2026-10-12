#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Containers/Ticker.h"
#include "TabGuardSubsystem.generated.h"

class IAssetEditorInstance;

/**
 * UTabGuardSubsystem — crash-safe snapshot of open asset tabs.
 * Saves on every asset open/close (debounced 0.5s) into Saved/TabGuard/TabSnapshot.json,
 * which the engine never touches, so it survives layout wipes and crashes.
 */
UCLASS()
class TABGUARD_API UTabGuardSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Write current open-asset list to snapshot file. Safe to call often (coalesced). */
	UFUNCTION(BlueprintCallable, Category = "TabGuard")
	void SaveSnapshot();

	/** Reopen every asset listed in the snapshot file. Skips already-open and missing assets. */
	UFUNCTION(BlueprintCallable, Category = "TabGuard")
	void RestoreSnapshot();

	/** Number of assets stored in snapshot, or 0 if no snapshot yet. */
	UFUNCTION(BlueprintCallable, Category = "TabGuard")
	int32 GetSnapshotAssetCount() const;

	static FString GetSnapshotFilePath();
	static FString GetBackupFilePath();

private:
	void BindAssetDelegates();
	void ScheduleSave();
	bool HandleDeferredSave(float DeltaTime);
	void OnAssetOpened(UObject* Asset, IAssetEditorInstance* Instance);
	void OnAssetClosed(UObject* Asset, IAssetEditorInstance* Instance);

	FDelegateHandle PostEngineInitHandle;
	FDelegateHandle AssetOpenedHandle;
	FDelegateHandle AssetClosedHandle;
	FTSTicker::FDelegateHandle DeferredSaveHandle;
	bool bSavePending = false;
	bool bDelegatesBound = false;
};
