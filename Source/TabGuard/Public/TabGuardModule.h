#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FTabGuardModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterMenus();

	static void OnSaveTabsClicked();
	static void OnRestoreTabsClicked();

	FDelegateHandle ToolMenusStartupHandle;
};
