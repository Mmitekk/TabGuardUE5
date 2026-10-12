#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class UToolMenu;

class FTabGuardModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterMenus();

	// Lazily builds the slots menu (called every time the dropdown opens, so labels stay fresh).
	static void FillSlotsMenu(UToolMenu* Menu);

	static void OnSaveTabsClicked();
	static void OnRestoreTabsClicked();
	static void OnSaveToSlotClicked(int32 SlotIndex);
	static void OnRestoreSlotClicked(int32 SlotIndex);
	static bool CanRestoreSlot(int32 SlotIndex);

	FDelegateHandle ToolMenusStartupHandle;
};
