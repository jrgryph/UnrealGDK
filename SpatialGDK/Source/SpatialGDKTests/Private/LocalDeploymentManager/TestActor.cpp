// Copyright (c) Improbable Worlds Ltd, All Rights Reserved

#include "TestActor.h"
//#include "GDKLogging.h"
//#include "Components/EquippedComponent.h"
//#include "Components/SkeletalMeshComponent.h"
//#include "UnrealNetwork.h"


ATestActor::ATestActor()
{
 	// Default to not ticking
	PrimaryActorTick.bCanEverTick = false;

	bReplicates = true;
	bReplicateMovement = true;

	//LocationComponent = CreateDefaultSubobject<USceneComponent>(TEXT("RootComponent"));
	//SetRootComponent(LocationComponent);

	//Mesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("WeaponMesh"));
	//Mesh->SetupAttachment(RootComponent);
}

bool ATestActor::DoSomethingOnServer_Validate()
{
	return true;
}

void ATestActor::DoSomethingOnServer_Implementation()
{
	UE_LOG(LogTemp, Warning, TEXT("Calling Server Function"));
}

void ATestActor::DoSomethingOnClient_Implementation()
{
	UE_LOG(LogTemp, Warning, TEXT("Calling Client Function"));
}

void ATestActor::ProcessEvent(UFunction* Function, void* Parameters)
{
	int32 FunctionCallspace = GetFunctionCallspace(Function, Parameters, NULL);
	if (FunctionCallspace & FunctionCallspace::Remote)
	{
		CallRemoteFunction(Function, Parameters, NULL, NULL);
	}
}
//void ATestActor::BeginPlay()
//{
//	Super::BeginPlay();
//	OnMetaDataUpdated();
//}
//
//void ATestActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
//{
//	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
//
//	DOREPLIFETIME(ATestActor, MetaData);
//	DOREPLIFETIME(ATestActor, CurrentMode);
//}
//
//void ATestActor::StartPrimaryUse_Implementation() { IsPrimaryUsing = true; }
//void ATestActor::StopPrimaryUse_Implementation() { IsPrimaryUsing = false; }
//void ATestActor::StartSecondaryUse_Implementation() { IsSecondaryUsing = true; }
//void ATestActor::StopSecondaryUse_Implementation() { IsSecondaryUsing = false; }
//
//void ATestActor::OnRep_MetaData()
//{
//	if (GetNetMode() == NM_DedicatedServer)
//	{
//		return;
//	}
//
//	OnMetaDataUpdated();
//}
//
//void ATestActor::SetMetaData(FGDKMetaData NewMetaData)
//{
//	if (HasAuthority())
//	{
//		MetaData = NewMetaData;
//	}
//	OnMetaDataUpdated();
//}
//
//void ATestActor::SetIsActive_Implementation(bool bNewIsActive)
//{
//	//TODO Find logic for sheathing inactive weapons
//	bIsActive = bNewIsActive;
//	this->SetActorHiddenInGame(!bNewIsActive);
//	StopPrimaryUse();
//	StopSecondaryUse();
//}
//
//FVector ATestActor::EffectSpawnPoint()
//{
//	return Mesh->GetSocketLocation(EffectSocketName);
//}
//
//void ATestActor::SetFirstPerson_Implementation(bool bNewFirstPerson)
//{
//	bIsFirstPerson = bNewFirstPerson;
//	Mesh->CastShadow = !bNewFirstPerson;
//}
//
//void ATestActor::ToggleMode_Implementation()
//{
//
//}
