#include "TabGuardModule.h"

#include "TabGuardSubsystem.h"
#include "ToolMenus.h"
#include "ToolMenu.h"
#include "ToolMenuSection.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Styling/AppStyle.h"
#include "Editor.h"
#include "Misc/DateTime.h"

#define LOCTEXT_NAMESPACE "TabGuard"

void FTabGuardModule::StartupModule()
{
	ToolMenusStartupHandle = UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FTabGuardModule::RegisterMenus));
}

void FTabGuardModule::ShutdownModule()
{
	if (UToolMenus::TryGet())
	{
		UToolMenus::UnRegisterStartupCallback(ToolMenusStartupHandle);
	}
}

void FTabGuardModule::RegisterMenus()
{
	const FNewToolMenuChoice SlotsMenuChoice(FNewToolMenuDelegate::CreateStatic(&FTabGuardModule::FillSlotsMenu));

	// 1) Level Editor user toolbar — always visible, one click away.
	if (UToolMenu* Toolbar = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.LevelEditorToolBar.User")))
	{
		FToolMenuSection& Section = Toolbar->FindOrAddSection(TEXT("TabGuard"));

		Section.AddEntry(FToolMenuEntry::InitToolBarButton(
			TEXT("TabGuard_Save"),
			FUIAction(FExecuteAction::CreateStatic(&FTabGuardModule::OnSaveTabsClicked)),
			LOCTEXT("TabGuard_Save_Label", "Save Tabs"),
			LOCTEXT("TabGuard_Save_Tooltip", "TabGuard: save currently open asset tabs now (auto-save already runs on every open/close)."),
			FSlateIcon()));

		Section.AddEntry(FToolMenuEntry::InitToolBarButton(
			TEXT("TabGuard_Restore"),
			FUIAction(FExecuteAction::CreateStatic(&FTabGuardModule::OnRestoreTabsClicked)),
			LOCTEXT("TabGuard_Restore_Label", "Restore Tabs"),
			LOCTEXT("TabGuard_Restore_Tooltip", "TabGuard: reopen tabs from the last autosave snapshot (use after crash or layout wipe)."),
			FSlateIcon()));

		Section.AddEntry(FToolMenuEntry::InitComboButton(
			TEXT("TabGuard_Slots"),
			FUIAction(),
			SlotsMenuChoice,
			LOCTEXT("TabGuard_Slots_Label", "Slots"),
			LOCTEXT("TabGuard_Slots_Tooltip", "TabGuard slots: save the current set of tabs into one of 5 slots, or restore a set from a slot."),
			FSlateIcon()));
	}

	// 2) Main Window menu — discoverable even if the toolbar is customized away.
	if (UToolMenu* WindowMenu = UToolMenus::Get()->ExtendMenu(TEXT("MainFrame.MainMenu.Window")))
	{
		FToolMenuSection& Section = WindowMenu->FindOrAddSection(TEXT("TabGuard"));

		Section.AddMenuEntry(
			TEXT("TabGuard_Save"),
			LOCTEXT("TabGuard_Save_Label", "Save Tabs"),
			LOCTEXT("TabGuard_Save_Tooltip", "TabGuard: save currently open asset tabs now (auto-save already runs on every open/close)."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateStatic(&FTabGuardModule::OnSaveTabsClicked)));

		Section.AddMenuEntry(
			TEXT("TabGuard_Restore"),
			LOCTEXT("TabGuard_Restore_Label", "Restore Tabs"),
			LOCTEXT("TabGuard_Restore_Tooltip", "TabGuard: reopen tabs from the last autosave snapshot (use after crash or layout wipe)."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateStatic(&FTabGuardModule::OnRestoreTabsClicked)));

		Section.AddSubMenu(
			TEXT("TabGuard_Slots"),
			LOCTEXT("TabGuard_Slots_Label", "TabGuard Slots"),
			LOCTEXT("TabGuard_Slots_Tooltip", "TabGuard slots: save the current set of tabs into one of 5 slots, or restore a set from a slot."),
			SlotsMenuChoice);
	}
}

void FTabGuardModule::FillSlotsMenu(UToolMenu* Menu)
{
	if (!Menu)
	{
		return;
	}

	FToolMenuSection& SaveSection = Menu->AddSection(
		TEXT("TabGuard_Slots_Save"),
		LOCTEXT("TabGuard_Slots_SaveSection", "Save Current Tabs To Slot"));
	FToolMenuSection& LoadSection = Menu->AddSection(
		TEXT("TabGuard_Slots_Load"),
		LOCTEXT("TabGuard_Slots_LoadSection", "Load Tabs From Slot"));

	for (int32 SlotIndex = 0; SlotIndex < UTabGuardSubsystem::MaxSlots; ++SlotIndex)
	{
		const FText SlotNumber = FText::AsNumber(SlotIndex + 1);

		FTabGuardSlotInfo Info;
		const bool bHasContent = UTabGuardSubsystem::GetSlotInfo(SlotIndex, Info) && Info.bExists;

		FText SavedAtText;
		if (bHasContent && Info.SavedAtUtc != FDateTime())
		{
			SavedAtText = FText::FromString(Info.SavedAtUtc.ToString(TEXT("%d.%m.%Y %H:%M UTC")));
		}

		// --- Save entry: always enabled, tooltip shows what would be overwritten.
		FText SaveTooltip = bHasContent
			? FText::Format(LOCTEXT("TabGuard_SlotSaveTip_Full", "Save all currently open tabs into Slot {0}.\nOverwrites the {1} tab(s) stored there{2}."),
				SlotNumber, FText::AsNumber(Info.AssetCount),
				SavedAtText.IsEmpty() ? FText::GetEmpty() : FText::Format(LOCTEXT("TabGuard_Slot_At", " (saved {0})"), SavedAtText))
			: FText::Format(LOCTEXT("TabGuard_SlotSaveTip_Empty", "Save all currently open tabs into Slot {0} (currently empty)."), SlotNumber);

		SaveSection.AddMenuEntry(
			FName(TEXT("TabGuard_SaveToSlot"), SlotIndex),
			FText::Format(LOCTEXT("TabGuard_SlotSave_Label", "Save to Slot {0}"), SlotNumber),
			SaveTooltip,
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateStatic(&FTabGuardModule::OnSaveToSlotClicked, SlotIndex)));

		// --- Load entry: disabled while the slot is empty; label shows content and time.
		FText LoadLabel = bHasContent
			? FText::Format(LOCTEXT("TabGuard_SlotLoad_Label", "Slot {0} — {1} tab(s){2}"),
				SlotNumber, FText::AsNumber(Info.AssetCount),
				SavedAtText.IsEmpty() ? FText::GetEmpty() : FText::Format(LOCTEXT("TabGuard_Slot_CommaAt", ", {0}"), SavedAtText))
			: FText::Format(LOCTEXT("TabGuard_SlotLoad_Empty", "Slot {0} — empty"), SlotNumber);

		LoadSection.AddMenuEntry(
			FName(TEXT("TabGuard_LoadSlot"), SlotIndex),
			LoadLabel,
			bHasContent
				? FText::Format(LOCTEXT("TabGuard_SlotLoadTip", "Reopen the {0} tab(s) stored in Slot {1}. Already-open tabs are skipped."),
					FText::AsNumber(Info.AssetCount), SlotNumber)
				: FText::Format(LOCTEXT("TabGuard_SlotLoadTip_Empty", "Slot {0} has nothing stored yet. Use \"Save to Slot {0}\" first."), SlotNumber),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateStatic(&FTabGuardModule::OnRestoreSlotClicked, SlotIndex),
				FCanExecuteAction::CreateStatic(&FTabGuardModule::CanRestoreSlot, SlotIndex)));
	}
}

void FTabGuardModule::OnSaveTabsClicked()
{
	if (!GEditor)
	{
		return;
	}
	if (UTabGuardSubsystem* Subsystem = GEditor->GetEditorSubsystem<UTabGuardSubsystem>())
	{
		Subsystem->SaveSnapshot();
	}
}

void FTabGuardModule::OnRestoreTabsClicked()
{
	if (!GEditor)
	{
		return;
	}
	if (UTabGuardSubsystem* Subsystem = GEditor->GetEditorSubsystem<UTabGuardSubsystem>())
	{
		Subsystem->RestoreSnapshot();
	}
}

void FTabGuardModule::OnSaveToSlotClicked(int32 SlotIndex)
{
	if (!GEditor)
	{
		return;
	}
	if (UTabGuardSubsystem* Subsystem = GEditor->GetEditorSubsystem<UTabGuardSubsystem>())
	{
		Subsystem->SaveSnapshotToSlot(SlotIndex);
	}
}

void FTabGuardModule::OnRestoreSlotClicked(int32 SlotIndex)
{
	if (!GEditor)
	{
		return;
	}
	if (UTabGuardSubsystem* Subsystem = GEditor->GetEditorSubsystem<UTabGuardSubsystem>())
	{
		Subsystem->RestoreSnapshotFromSlot(SlotIndex);
	}
}

bool FTabGuardModule::CanRestoreSlot(int32 SlotIndex)
{
	FTabGuardSlotInfo Info;
	return UTabGuardSubsystem::GetSlotInfo(SlotIndex, Info) && Info.bExists && Info.AssetCount > 0;
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FTabGuardModule, TabGuard)
