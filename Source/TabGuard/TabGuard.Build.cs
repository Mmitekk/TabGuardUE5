using UnrealBuildTool;

public class TabGuard : ModuleRules
{
	public TabGuard(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Slate",
			"SlateCore",
			"UnrealEd",
			"LevelEditor",
			"ToolMenus",
			"EditorSubsystem",
			"Projects",
			"Json",
			"JsonUtilities"
		});
	}
}
