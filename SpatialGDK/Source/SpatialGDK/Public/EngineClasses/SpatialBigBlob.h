// Copyright (c) Improbable Worlds Ltd, All Rights Reserved

#pragma once

#include "TimerManager.h"
//#include "CoreMinimal.h"

#include "SpatialBigBlob.generated.h"

class USpatialWorkerConnection;
class USpatialDispatcher;
class USpatialSender;
class USpatialReceiver;
class UActorGroupManager;
class USpatialClassInfoManager;
class UGlobalStateManager;
class USpatialPlayerSpawner;
class USpatialPackageMapClient;
class USpatialStaticComponentView;
class USnapshotManager;
class UEntityPool;
class USpatialMetrics;
class ASpatialMetricsDisplay;

UCLASS()
class SPATIALGDK_API USpatialBigBlob : public UObject
{
	GENERATED_BODY()

public:

	USpatialBigBlob(const FObjectInitializer& ObjectInitializer);

	UPROPERTY()
	USpatialWorkerConnection* Connection;
	UPROPERTY()
	USpatialDispatcher* Dispatcher;
	UPROPERTY()
	USpatialSender* Sender;
	UPROPERTY()
	USpatialReceiver* Receiver;
	UPROPERTY()
	UActorGroupManager* ActorGroupManager;
	UPROPERTY()
	USpatialClassInfoManager* ClassInfoManager;
	UPROPERTY()
	UGlobalStateManager* GlobalStateManager;
	UPROPERTY()
	USpatialPlayerSpawner* PlayerSpawner;
	UPROPERTY()
	USpatialPackageMapClient* PackageMap;
	UPROPERTY()
	USpatialStaticComponentView* StaticComponentView;
	UPROPERTY()
	USnapshotManager* SnapshotManager;
	UPROPERTY()
	UEntityPool* EntityPool;
	UPROPERTY()
	USpatialMetrics* SpatialMetrics;
	UPROPERTY()
	ASpatialMetricsDisplay* SpatialMetricsDisplay;

	FTimerManager TimerManager;
};
