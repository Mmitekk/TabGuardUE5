#include "TabGuardModule.h"

#include "TabGuardSubsystem.h"
#include "ToolMenus.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Styling/AppStyle.h"
#include "Editor.h"

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
			LOCTEXT("TabGuard_Restore_Tooltip", "TabGuard: reopen tabs from the last snapshot (use after crash or layout wipe)."),
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
			LOCTEXT("TabGuard_Restore_Tooltip", "TabGuard: reopen tabs from the last snapshot (use after crash or layout wipe)."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateStatic(&FTabGuardModule::OnRestoreTabsClicked)));
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

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FTabGuardModule, TabGuard)
