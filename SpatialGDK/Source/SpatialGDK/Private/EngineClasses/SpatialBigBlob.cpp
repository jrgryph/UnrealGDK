// Copyright (c) Improbable Worlds Ltd, All Rights Reserved

#include "EngineClasses/SpatialBigBlob.h"

#include "EngineClasses/SpatialPackageMapClient.h"
#include "Interop/Connection/SpatialWorkerConnection.h"
#include "Interop/GlobalStateManager.h"
#include "Interop/SnapshotManager.h"
#include "Interop/SpatialClassInfoManager.h"
#include "Interop/SpatialDispatcher.h"
#include "Interop/SpatialPlayerSpawner.h"
#include "Interop/SpatialReceiver.h"
#include "Interop/SpatialSender.h"
#include "Interop/SpatialStaticComponentView.h"
#include "Utils/ActorGroupManager.h"
#include "Utils/EntityPool.h"
#include "Utils/SpatialMetrics.h"
#include "Utils/SpatialMetricsDisplay.h"

USpatialBigBlob::USpatialBigBlob(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}
