// Copyright (c) Improbable Worlds Ltd, All Rights Reserved

#include "TestDefinitions.h"

#include "TestActor.h"

#include "LocalDeploymentManager.h"
#include "SpatialGDKDefaultLaunchConfigGenerator.h"
#include "SpatialGDKDefaultWorkerJsonGenerator.h"
#include "SpatialGDKEditorSettings.h"

#include "EngineClasses/SpatialNetDriver.h"
#include "Interop/Connection/SpatialWorkerConnection.h"

//#include "AutomationEditorCommon.h"
//#include "AutomationCommon.h"
#include "LevelEditor.h"
#include "UnrealEdGlobals.h"
#include "UnrealEd/Classes/Editor/UnrealEdEngine.h"


#include "CoreMinimal.h"

#define LOCALDEPLOYMENT_TEST(TestName) \
	TEST(Services, LocalDeployment, TestName)

namespace
{
	// TODO: UNR-1969 - Prepare LocalDeployment in CI pipeline
	const double MAX_WAIT_TIME_FOR_LOCAL_DEPLOYMENT_OPERATION = 10.0;

	// TODO: UNR-1964 - Move EDeploymentState enum to LocalDeploymentManager
	enum class EDeploymentState { IsRunning, IsNotRunning };

	const FName AutomationWorkerType = TEXT("AutomationWorker");
	const FString AutomationLaunchConfig = TEXT("Improbable/AutomationLaunchConfig.json");

	FLocalDeploymentManager* GetLocalDeploymentManager()
	{
		FSpatialGDKServicesModule& GDKServices = FModuleManager::GetModuleChecked<FSpatialGDKServicesModule>("SpatialGDKServices");
		FLocalDeploymentManager* LocalDeploymentManager = GDKServices.GetLocalDeploymentManager();
		return LocalDeploymentManager;
	}

	bool GenerateWorkerAssemblies()
	{
		FString BuildConfigArgs = TEXT("worker build build-config");
		FString WorkerBuildConfigResult;
		int32 ExitCode;
		const FString SpatialExe(TEXT("spatial.exe"));
		FSpatialGDKServicesModule::ExecuteAndReadOutput(SpatialExe, BuildConfigArgs, FSpatialGDKServicesModule::GetSpatialOSDirectory(), WorkerBuildConfigResult, ExitCode);

		const int32 ExitCodeSuccess = 0;
		return (ExitCode == ExitCodeSuccess);
	}

	bool GenerateWorkerJson()
	{
		const FString WorkerJsonDir = FSpatialGDKServicesModule::GetSpatialOSDirectory(TEXT("workers/unreal"));

		FString JsonPath = FPaths::Combine(WorkerJsonDir, TEXT("spatialos.UnrealAutomation.worker.json"));
		if (!FPaths::FileExists(JsonPath))
		{
			bool bRedeployRequired = false;
			return GenerateDefaultWorkerJson(JsonPath, AutomationWorkerType.ToString(), bRedeployRequired);
		}

		return true;
	}
}

DEFINE_LATENT_COMMAND(StartDeployment)
{
	if (const USpatialGDKEditorSettings* SpatialGDKSettings = GetDefault<USpatialGDKEditorSettings>())
	{
		FLocalDeploymentManager* LocalDeploymentManager = GetLocalDeploymentManager();
		const FString LaunchConfig = FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()), AutomationLaunchConfig);
		const FString LaunchFlags = SpatialGDKSettings->GetSpatialOSCommandLineLaunchFlags();
		const FString SnapshotName = SpatialGDKSettings->GetSpatialOSSnapshotToLoad();

		AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [LocalDeploymentManager, LaunchConfig, LaunchFlags, SnapshotName]
		{
			if (!GenerateWorkerJson())
			{
				return;
			}

			if (!GenerateWorkerAssemblies())
			{
				return;
			}

			FSpatialLaunchConfigDescription LaunchConfigDescription(AutomationWorkerType);

			if (!GenerateDefaultLaunchConfig(LaunchConfig, &LaunchConfigDescription))
			{
				return;
			}

			if (LocalDeploymentManager->IsLocalDeploymentRunning())
			{
				return;
			}

			LocalDeploymentManager->TryStartLocalDeployment(LaunchConfig, LaunchFlags, SnapshotName);
		});
	}

	return true;
}

DEFINE_LATENT_COMMAND(StopDeployment)
{
	FLocalDeploymentManager* LocalDeploymentManager = GetLocalDeploymentManager();

	if (!LocalDeploymentManager->IsLocalDeploymentRunning() && !LocalDeploymentManager->IsDeploymentStopping())
	{
		return true;
	}

	if (!LocalDeploymentManager->IsDeploymentStopping())
	{
		AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [LocalDeploymentManager]
		{
			LocalDeploymentManager->TryStopLocalDeployment();
		});
	}

	return true;
}

DEFINE_LATENT_COMMAND_ONE_PARAMETER(WaitFor, double, WaitTime)
{
	const double NewTime = FPlatformTime::Seconds();
	if (NewTime - StartTime >= WaitTime)
	{
		return true;
	}
	else
	{
		return false;
	}
}

DEFINE_LATENT_COMMAND_TWO_PARAMETERS(WaitForDeployment, FAutomationTestBase*, Test, EDeploymentState, ExpectedDeploymentState)
{
	const double NewTime = FPlatformTime::Seconds();
	if (NewTime - StartTime >= MAX_WAIT_TIME_FOR_LOCAL_DEPLOYMENT_OPERATION)
	{
		return true;
	}

	FLocalDeploymentManager* LocalDeploymentManager = GetLocalDeploymentManager();
	if (LocalDeploymentManager->IsDeploymentStopping())
	{
		return false;
	}
	else
	{
		return (ExpectedDeploymentState == EDeploymentState::IsRunning) ? LocalDeploymentManager->IsLocalDeploymentRunning() : !LocalDeploymentManager->IsLocalDeploymentRunning();
	}
}

DEFINE_LATENT_COMMAND_TWO_PARAMETERS(CheckDeploymentState, FAutomationTestBase*, Test, EDeploymentState, ExpectedDeploymentState)
{
	FLocalDeploymentManager* LocalDeploymentManager = GetLocalDeploymentManager();

	if (ExpectedDeploymentState == EDeploymentState::IsRunning)
	{
		Test->TestTrue(TEXT("Deployment is running"), LocalDeploymentManager->IsLocalDeploymentRunning() && !LocalDeploymentManager->IsDeploymentStopping());
	}
	else
	{
		Test->TestFalse(TEXT("Deployment is not running"), LocalDeploymentManager->IsLocalDeploymentRunning() || LocalDeploymentManager->IsDeploymentStopping());
	}

	return true;
}

LOCALDEPLOYMENT_TEST(GIVEN_no_deployment_running_WHEN_deployment_started_THEN_deployment_running)
{
	// GIVEN
	ADD_LATENT_AUTOMATION_COMMAND(StopDeployment());
	ADD_LATENT_AUTOMATION_COMMAND(WaitForDeployment(this, EDeploymentState::IsNotRunning));

	// WHEN
	ADD_LATENT_AUTOMATION_COMMAND(StartDeployment());
	ADD_LATENT_AUTOMATION_COMMAND(WaitForDeployment(this, EDeploymentState::IsRunning));

	// THEN
	ADD_LATENT_AUTOMATION_COMMAND(CheckDeploymentState(this, EDeploymentState::IsRunning));

	// Cleanup
	ADD_LATENT_AUTOMATION_COMMAND(StopDeployment());
	ADD_LATENT_AUTOMATION_COMMAND(WaitForDeployment(this, EDeploymentState::IsNotRunning));
    return true;
}

LOCALDEPLOYMENT_TEST(GIVEN_deployment_running_WHEN_deployment_stopped_THEN_deployment_not_running)
{
	// GIVEN
	ADD_LATENT_AUTOMATION_COMMAND(StopDeployment());
	ADD_LATENT_AUTOMATION_COMMAND(WaitForDeployment(this, EDeploymentState::IsNotRunning));
	ADD_LATENT_AUTOMATION_COMMAND(StartDeployment());
	ADD_LATENT_AUTOMATION_COMMAND(WaitForDeployment(this, EDeploymentState::IsRunning));

	// WHEN
	ADD_LATENT_AUTOMATION_COMMAND(StopDeployment());
	ADD_LATENT_AUTOMATION_COMMAND(WaitForDeployment(this, EDeploymentState::IsNotRunning));

	// THEN
	ADD_LATENT_AUTOMATION_COMMAND(CheckDeploymentState(this, EDeploymentState::IsNotRunning));
    return true;
}

static USpatialNetDriver* Driver = nullptr;
static USpatialWorkerConnection* SpatialConnection = nullptr;

DEFINE_LATENT_COMMAND(CreateDriverAndConnection)
{
	{
		//USpatialNetDriver* Driver = NewObject<USpatialNetDriver>();
		Driver = NewObject<USpatialNetDriver>();
		FString Error;
		FURL URL;
		URL.Protocol = TEXT("unreal");
		URL.Port = 7777;
		URL.Valid = 1;
		URL.Map = TEXT("/Game/Maps/UEDPIE_1_FPS-Start_Tiny");

		SpatialConnection = NewObject<USpatialWorkerConnection>();
		SpatialConnection->Init(nullptr);

		Driver->WorkerConnection = SpatialConnection;
		SpatialConnection->SetSpatialNetDriver(Driver);

		Driver->InitBase(false, nullptr, URL, false, Error);
		SpatialConnection->Connect(false);
	}

	return true;
}

DEFINE_LATENT_COMMAND(WaitHere)
{
	//SpatialConnection->SendMetrics();
	SpatialConnection;
	Driver;
	//ATestActor* TestActor = NewObject<ATestActor>();
	//ATestActor* TestActor = GUnrealEd->GetWorld()->SpawnActor<ATestActor>();
	FURL URL;
	GWorld->InitializeActorsForPlay(URL);
	GWorld->BeginPlay();
	ATestActor* TestActor = GWorld->SpawnActor<ATestActor>();
	TestActor->DoSomethingOnClient();
	TestActor->DoSomethingOnServer();
	//void* Parameters = nullptr;
	//struct FOutParmRec* = nullptr;
	//Driver->ProcessRemoteFunction(TestActor, ATestActor::DoSomethingOnClient, Parameters, OutParms);

	//virtual void ProcessRemoteFunction(class AActor* Actor, class UFunction* Function, void* Parameters, struct FOutParmRec* OutParms, struct FFrame* NotStack, class UObject* SubObject = NULL ) override;
//void USpatialNetDriver::ProcessRPC(AActor* Actor, UObject* SubObject, UFunction* Function, void* Parameters)
	return true;
}

DEFINE_LATENT_COMMAND_ONE_PARAMETER(StartPlaySession, bool, bSimulateInEditor)
{
	FLevelEditorModule& LevelEditorModule = FModuleManager::Get().GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
	TSharedPtr<class ILevelViewport> ActiveLevelViewport = LevelEditorModule.GetFirstActiveViewport();

	GUnrealEd->RequestPlaySession(false, ActiveLevelViewport, bSimulateInEditor, NULL, NULL, -1, false);
	return true;
}

DEFINE_LATENT_COMMAND(StopPlaySession)
{
	GUnrealEd->RequestEndPlayMap();
	return true;
}

LOCALDEPLOYMENT_TEST(Sometest)
{
	ADD_LATENT_AUTOMATION_COMMAND(StartDeployment());
	ADD_LATENT_AUTOMATION_COMMAND(WaitForDeployment(this, EDeploymentState::IsRunning));
	ADD_LATENT_AUTOMATION_COMMAND(WaitFor(10.0));
	ADD_LATENT_AUTOMATION_COMMAND(CreateDriverAndConnection());
	ADD_LATENT_AUTOMATION_COMMAND(WaitFor(10.0));
	ADD_LATENT_AUTOMATION_COMMAND(WaitHere());
    return true;
}

LOCALDEPLOYMENT_TEST(MyServerPie)
{
	ADD_LATENT_AUTOMATION_COMMAND(StartPlaySession(true));
	ADD_LATENT_AUTOMATION_COMMAND(WaitFor(30.0f));
	ADD_LATENT_AUTOMATION_COMMAND(StopPlaySession);

	return true;
}

LOCALDEPLOYMENT_TEST(MyClientPie)
{
	ADD_LATENT_AUTOMATION_COMMAND(StartPlaySession(false));
	ADD_LATENT_AUTOMATION_COMMAND(WaitFor(15.0f));
	ADD_LATENT_AUTOMATION_COMMAND(WaitHere());
	ADD_LATENT_AUTOMATION_COMMAND(StopPlaySession);

	return true;
}
