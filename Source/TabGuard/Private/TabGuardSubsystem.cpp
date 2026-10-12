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

	UE_LOG(LogTemp, Log, TEXT("TabGuard: tracking asset tabs (snapshot: %s)"), *GetSnapshotFilePath());
}

void UTabGuardSubsystem::OnAssetOpened(UObject* Asset, IAssetEditorInstance* Instance)
{
	ScheduleSave();
}

void UTabGuardSubsystem::OnAssetClosed(UObject* Asset, IAssetEditorInstance* Instance)
{
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

void UTabGuardSubsystem::SaveSnapshot()
{
	bSavePending = false;

	if (!GEditor)
	{
		return;
	}
	UAssetEditorSubsystem* AssetEditor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditor)
	{
		return;
	}

	TArray<UObject*> EditedAssets = AssetEditor->GetAllEditedAssets();

	TSet<FString> UniquePaths;
	TArray<FString> SortedPaths;
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
		FString Path = Asset->GetPathName();
		if (Path.IsEmpty() || Path.StartsWith(TEXT("/Temp/")))
		{
			continue;
		}
		bool bIsNew = false;
		UniquePaths.Add(Path, &bIsNew);
		if (bIsNew)
		{
			SortedPaths.Add(Path);
		}
	}
	SortedPaths.Sort();

	const FString SnapshotPath = GetSnapshotFilePath();
	const FString BackupPath = GetBackupFilePath();

	// Rotate: keep previous snapshot as .bak before overwriting.
	IFileManager& FileManager = IFileManager::Get();
	FileManager.MakeDirectory(*FPaths::GetPath(SnapshotPath), true);
	if (FileManager.FileExists(*SnapshotPath))
	{
		FileManager.Copy(*BackupPath, *SnapshotPath, true, true);
	}

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
		UE_LOG(LogTemp, Warning, TEXT("TabGuard: failed to serialize snapshot"));
		return;
	}

	if (FFileHelper::SaveStringToFile(OutString, *SnapshotPath))
	{
		UE_LOG(LogTemp, Log, TEXT("TabGuard: saved %d tab(s) to %s"), SortedPaths.Num(), *SnapshotPath);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("TabGuard: failed to write %s"), *SnapshotPath);
	}
}

void UTabGuardSubsystem::RestoreSnapshot()
{
	if (!GEditor)
	{
		return;
	}
	UAssetEditorSubsystem* AssetEditor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditor)
	{
		return;
	}

	const FString SnapshotPath = GetSnapshotFilePath();
	FString FileContent;
	if (!FFileHelper::LoadFileToString(FileContent, *SnapshotPath))
	{
		FNotificationInfo Info(LOCTEXT("TabGuard_NoSnapshot", "TabGuard: no snapshot found yet. Open some assets first — they are saved automatically."));
		Info.ExpireDuration = 5.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
		UE_LOG(LogTemp, Warning, TEXT("TabGuard: no snapshot at %s"), *SnapshotPath);
		return;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContent);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		FNotificationInfo Info(LOCTEXT("TabGuard_BadSnapshot", "TabGuard: snapshot file is corrupted."));
		Info.ExpireDuration = 5.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
		UE_LOG(LogTemp, Warning, TEXT("TabGuard: corrupted snapshot at %s"), *SnapshotPath);
		return;
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
	int32 Skipped = 0;
	for (const FString& Path : SavedPaths)
	{
		if (OpenPaths.Contains(Path))
		{
			++Skipped;
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

	FText Message = FText::Format(
		LOCTEXT("TabGuard_Restored", "TabGuard: reopened {0} tab(s), {1} already open."),
		FText::AsNumber(Restored), FText::AsNumber(Skipped));
	FNotificationInfo Info(Message);
	Info.ExpireDuration = 5.0f;
	FSlateNotificationManager::Get().AddNotification(Info);

	UE_LOG(LogTemp, Log, TEXT("TabGuard: restored %d tab(s), skipped %d already-open (from %d saved)"),
		Restored, Skipped, SavedPaths.Num());
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
