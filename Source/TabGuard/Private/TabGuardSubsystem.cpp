#include "TabGuardSubsystem.h"

#include "Editor.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/DateTime.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/CoreDelegates.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "TabGuard"

FString UTabGuardSubsystem::GetSnapshotFilePath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("TabGuard/TabSnapshot.json"));
}

FString UTabGuardSubsystem::GetBackupFilePath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("TabGuard/TabSnapshot.bak.json"));
}

FString UTabGuardSubsystem::GetSlotFilePath(int32 SlotIndex)
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("TabGuard/Slots"),
		FString::Printf(TEXT("Slot%d.json"), SlotIndex + 1));
}

void UTabGuardSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// AssetEditorSubsystem may not exist yet at Initialize time — defer binding.
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddUObject(this, &UTabGuardSubsystem::BindAssetDelegates);
	// In case we are initialized late (hot reload), try binding right away too.
	BindAssetDelegates();
}

void UTabGuardSubsystem::Deinitialize()
{
	FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);

	if (bDelegatesBound && GEditor)
	{
		if (UAssetEditorSubsystem* AssetEditor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			AssetEditor->OnAssetOpenedInEditor().Remove(AssetOpenedHandle);
			AssetEditor->OnAssetClosedInEditor().Remove(AssetClosedHandle);
		}
	}
	bDelegatesBound = false;

	if (DeferredSaveHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(DeferredSaveHandle);
		DeferredSaveHandle.Reset();
	}

	Super::Deinitialize();
}

void UTabGuardSubsystem::BindAssetDelegates()
{
	if (bDelegatesBound)
	{
		return;
	}
	if (!GEditor)
	{
		return;
	}
	UAssetEditorSubsystem* AssetEditor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditor)
	{
		return;
	}

	AssetOpenedHandle = AssetEditor->OnAssetOpenedInEditor().AddUObject(this, &UTabGuardSubsystem::OnAssetOpened);
	AssetClosedHandle = AssetEditor->OnAssetClosedInEditor().AddUObject(this, &UTabGuardSubsystem::OnAssetClosed);
	bDelegatesBound = true;

	// Seed own tracking with whatever the engine map already has (assets opened before we bound).
	for (UObject* Asset : AssetEditor->GetAllEditedAssets())
	{
		if (IsValid(Asset) && Asset->GetOutermost() != GetTransientPackage())
		{
			const FString Path = Asset->GetPathName();
			if (IsSnapshotablePath(Path))
			{
				TrackedOpenAssetPaths.Add(Path);
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("TabGuard: tracking asset tabs (snapshot: %s)"), *GetSnapshotFilePath());
}

bool UTabGuardSubsystem::IsSnapshotablePath(const FString& Path)
{
	return !Path.IsEmpty()
		&& !Path.StartsWith(TEXT("/Temp/"))
		&& !Path.StartsWith(TEXT("/Engine/Transient"));
}

void UTabGuardSubsystem::OnAssetOpened(UObject* Asset, IAssetEditorInstance* Instance)
{
	if (IsValid(Asset) && Asset->GetOutermost() != GetTransientPackage())
	{
		const FString Path = Asset->GetPathName();
		if (IsSnapshotablePath(Path))
		{
			TrackedOpenAssetPaths.Add(Path);
		}
	}
	ScheduleSave();
}

void UTabGuardSubsystem::OnAssetClosed(UObject* Asset, IAssetEditorInstance* Instance)
{
	// At broadcast time the closing editor is still registered in the engine map,
	// so Num() == 1 means no other editor keeps this asset open.
	if (IsValid(Asset) && GEditor)
	{
		if (UAssetEditorSubsystem* AssetEditor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			if (AssetEditor->FindEditorsForAsset(Asset).Num() <= 1)
			{
				TrackedOpenAssetPaths.Remove(Asset->GetPathName());
			}
		}
	}
	ScheduleSave();
}

void UTabGuardSubsystem::ScheduleSave()
{
	if (bSavePending)
	{
		return;
	}
	bSavePending = true;
	// Debounce: OnAssetClosed fires before the editor list is updated,
	// and batch-opens fire N events in a row. One write per 0.5s is enough
	// and still crash-safe (survives even Alt+F4 + instant reboot).
	if (DeferredSaveHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(DeferredSaveHandle);
	}
	DeferredSaveHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UTabGuardSubsystem::HandleDeferredSave), 0.5f);
}

bool UTabGuardSubsystem::HandleDeferredSave(float DeltaTime)
{
	DeferredSaveHandle.Reset();
	bSavePending = false;
	SaveSnapshot();
	return false; // one-shot
}

bool UTabGuardSubsystem::WriteSnapshotToFile(const FString& FilePath)
{
	if (!GEditor)
	{
		return false;
	}
	UAssetEditorSubsystem* AssetEditor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditor)
	{
		return false;
	}

	TArray<UObject*> EditedAssets = AssetEditor->GetAllEditedAssets();

	TSet<FString> UniquePaths;
	TArray<FString> SortedPaths;

	// Source 1: the engine's map (weak pointers — can go stale, e.g. after object replacement).
	int32 EngineMapCount = 0;
	for (UObject* Asset : EditedAssets)
	{
		if (!IsValid(Asset))
		{
			continue;
		}
		// Skip memory-only / transient objects (e.g. unsaved pasted graphs).
		if (Asset->GetOutermost() == GetTransientPackage())
		{
			continue;
		}
		const FString Path = Asset->GetPathName();
		if (!IsSnapshotablePath(Path))
		{
			continue;
		}
		bool bIsNew = false;
		UniquePaths.Add(Path, &bIsNew);
		if (bIsNew)
		{
			SortedPaths.Add(Path);
			++EngineMapCount;
		}
	}

	// Source 2: own path tracking from open/close delegates (strings — immune to GC staleness).
	int32 TrackedCount = 0;
	for (const FString& Path : TrackedOpenAssetPaths)
	{
		if (!IsSnapshotablePath(Path))
		{
			continue;
		}
		bool bIsNew = false;
		UniquePaths.Add(Path, &bIsNew);
		if (bIsNew)
		{
			SortedPaths.Add(Path);
			++TrackedCount;
		}
	}
	SortedPaths.Sort();

	UE_LOG(LogTemp, Log, TEXT("TabGuard: save %s: %d from engine map, %d extra tracked, %d total"),
		*FilePath, EngineMapCount, TrackedCount, SortedPaths.Num());

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("Version"), 1);
	Root->SetStringField(TEXT("SavedAt"), FDateTime::UtcNow().ToIso8601());
	Root->SetStringField(TEXT("Project"), FApp::GetProjectName());

	TArray<TSharedPtr<FJsonValue>> JsonAssets;
	for (const FString& Path : SortedPaths)
	{
		JsonAssets.Add(MakeShared<FJsonValueString>(Path));
	}
	Root->SetArrayField(TEXT("Assets"), JsonAssets);

	FString OutString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutString);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		UE_LOG(LogTemp, Warning, TEXT("TabGuard: failed to serialize snapshot to %s"), *FilePath);
		return false;
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
	if (!FFileHelper::SaveStringToFile(OutString, *FilePath))
	{
		UE_LOG(LogTemp, Warning, TEXT("TabGuard: failed to write %s"), *FilePath);
		return false;
	}

	UE_LOG(LogTemp, Log, TEXT("TabGuard: saved %d tab(s) to %s"), SortedPaths.Num(), *FilePath);
	return true;
}

void UTabGuardSubsystem::SaveSnapshot()
{
	bSavePending = false;

	const FString SnapshotPath = GetSnapshotFilePath();

	// Rotate: keep previous snapshot as .bak before overwriting.
	if (IFileManager::Get().FileExists(*SnapshotPath))
	{
		IFileManager::Get().Copy(*GetBackupFilePath(), *SnapshotPath, true, true);
	}

	WriteSnapshotToFile(SnapshotPath);
}

int32 UTabGuardSubsystem::ReadSnapshotAndOpen(const FString& FilePath)
{
	if (!GEditor)
	{
		return -1;
	}
	UAssetEditorSubsystem* AssetEditor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditor)
	{
		return -1;
	}

	FString FileContent;
	if (!FFileHelper::LoadFileToString(FileContent, *FilePath))
	{
		return -1;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContent);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("TabGuard: corrupted snapshot at %s"), *FilePath);
		return -1;
	}

	TArray<FString> SavedPaths;
	for (const TSharedPtr<FJsonValue>& V : Root->GetArrayField(TEXT("Assets")))
	{
		FString P;
		if (V->TryGetString(P) && !P.IsEmpty())
		{
			SavedPaths.Add(P);
		}
	}

	// Skip assets that are already open.
	TSet<FString> OpenPaths;
	for (UObject* OpenAsset : AssetEditor->GetAllEditedAssets())
	{
		if (IsValid(OpenAsset))
		{
			OpenPaths.Add(OpenAsset->GetPathName());
		}
	}

	int32 Restored = 0;
	for (const FString& Path : SavedPaths)
	{
		if (OpenPaths.Contains(Path))
		{
			continue;
		}
		FSoftObjectPath SoftPath(Path);
		if (!SoftPath.IsValid())
		{
			continue;
		}
		// OpenEditorForAsset loads the asset if needed and brings the tab back.
		AssetEditor->OpenEditorForAsset(SoftPath);
		++Restored;
	}

	return Restored;
}

void UTabGuardSubsystem::RestoreSnapshot()
{
	const int32 Restored = ReadSnapshotAndOpen(GetSnapshotFilePath());
	if (Restored < 0)
	{
		FNotificationInfo Info(LOCTEXT("TabGuard_NoSnapshot", "TabGuard: no snapshot found yet. Open some assets first — they are saved automatically."));
		Info.ExpireDuration = 5.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
		UE_LOG(LogTemp, Warning, TEXT("TabGuard: no snapshot at %s"), *GetSnapshotFilePath());
		return;
	}

	FText Message = FText::Format(
		LOCTEXT("TabGuard_Restored", "TabGuard: reopened {0} tab(s) from autosave snapshot."),
		FText::AsNumber(Restored));
	FNotificationInfo Info(Message);
	Info.ExpireDuration = 5.0f;
	FSlateNotificationManager::Get().AddNotification(Info);

	UE_LOG(LogTemp, Log, TEXT("TabGuard: restored %d tab(s) from autosave snapshot"), Restored);
}

bool UTabGuardSubsystem::SaveSnapshotToSlot(int32 SlotIndex)
{
	if (SlotIndex < 0 || SlotIndex >= MaxSlots)
	{
		UE_LOG(LogTemp, Warning, TEXT("TabGuard: invalid slot index %d"), SlotIndex);
		return false;
	}

	if (!WriteSnapshotToFile(GetSlotFilePath(SlotIndex)))
	{
		return false;
	}

	FTabGuardSlotInfo Info;
	if (GetSlotInfo(SlotIndex, Info))
	{
		FText Message = FText::Format(
			LOCTEXT("TabGuard_SlotSaved", "TabGuard: {0} tab(s) saved to Slot {1}."),
			FText::AsNumber(Info.AssetCount), FText::AsNumber(SlotIndex + 1));
		FNotificationInfo Notification(Message);
		Notification.ExpireDuration = 4.0f;
		FSlateNotificationManager::Get().AddNotification(Notification);
	}
	return true;
}

bool UTabGuardSubsystem::RestoreSnapshotFromSlot(int32 SlotIndex)
{
	if (SlotIndex < 0 || SlotIndex >= MaxSlots)
	{
		UE_LOG(LogTemp, Warning, TEXT("TabGuard: invalid slot index %d"), SlotIndex);
		return false;
	}

	const int32 Restored = ReadSnapshotAndOpen(GetSlotFilePath(SlotIndex));
	if (Restored < 0)
	{
		FText Message = FText::Format(
			LOCTEXT("TabGuard_SlotEmpty", "TabGuard: Slot {0} is empty or corrupted."),
			FText::AsNumber(SlotIndex + 1));
		FNotificationInfo Info(Message);
		Info.ExpireDuration = 5.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
		return false;
	}

	FText Message = FText::Format(
		LOCTEXT("TabGuard_SlotRestored", "TabGuard: reopened {0} tab(s) from Slot {1} (already-open tabs skipped)."),
		FText::AsNumber(Restored), FText::AsNumber(SlotIndex + 1));
	FNotificationInfo Info(Message);
	Info.ExpireDuration = 5.0f;
	FSlateNotificationManager::Get().AddNotification(Info);

	return true;
}

int32 UTabGuardSubsystem::GetSlotAssetCount(int32 SlotIndex) const
{
	FTabGuardSlotInfo Info;
	return GetSlotInfo(SlotIndex, Info) ? Info.AssetCount : 0;
}

bool UTabGuardSubsystem::GetSlotInfo(int32 SlotIndex, FTabGuardSlotInfo& OutInfo)
{
	if (SlotIndex < 0 || SlotIndex >= MaxSlots)
	{
		return false;
	}

	const FString FilePath = GetSlotFilePath(SlotIndex);
	if (!IFileManager::Get().FileExists(*FilePath))
	{
		return false;
	}

	FString FileContent;
	if (!FFileHelper::LoadFileToString(FileContent, *FilePath))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContent);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return false;
	}

	OutInfo.bExists = true;
	OutInfo.AssetCount = Root->GetArrayField(TEXT("Assets")).Num();
	FString SavedAt;
	if (Root->TryGetStringField(TEXT("SavedAt"), SavedAt))
	{
		FDateTime::ParseIso8601(*SavedAt, OutInfo.SavedAtUtc);
	}
	return true;
}

int32 UTabGuardSubsystem::GetSnapshotAssetCount() const
{
	FString FileContent;
	if (!FFileHelper::LoadFileToString(FileContent, *GetSnapshotFilePath()))
	{
		return 0;
	}
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContent);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return 0;
	}
	return Root->GetArrayField(TEXT("Assets")).Num();
}

#undef LOCTEXT_NAMESPACE
