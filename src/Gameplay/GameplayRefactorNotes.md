# Gameplay refactor notes

This archive applies the first **structural gameplay refactor** without trying to solve every future gameplay problem at once.

## Main goals

- keep `LevelECS` / `Scene` as low-level world + render/runtime layers
- move gameplay-domain logic into `src/Gameplay`
- stop baking locomotion math directly into graph tasks
- keep the existing graph layer, but narrow it to orchestration / state transitions
- make room for future data-driven gameplay config without overengineering the first step

## New folder layout

- `Gameplay/GameplayWorld.cppm`
- `Gameplay/GameplayWorld_impl.cppm`
- `Gameplay/Input/GameplayInput.cppm`
- `Gameplay/Character/CharacterController.cppm`
- `Gameplay/Character/CharacterMovement.cppm`
- `Gameplay/Combat/CombatSystem.cppm`
- `Gameplay/Interaction/InteractionSystem.cppm`
- `Gameplay/Animation/GameplayAnimationBridge.cppm`
- `Gameplay/Animation/GameplayAnimationBridgeSystem.cppm`
- `Gameplay/Graph/GameplayGraph.cppm`
- `Gameplay/Graph/GameplayGraphAssets.cppm`
- `Gameplay/Runtime/GameplayRuntimeCommon.cppm`
- `Gameplay/Runtime/GameplayBootstrap.cppm`
- `Gameplay/Runtime/GameplaySceneSync.cppm`
- `Gameplay/Runtime/GameplayRuntime.cppm`

## Responsibility split

### Input
Reads raw input and fills `GameplayInputIntentComponent`.

### CharacterController
Converts intent into `GameplayCharacterCommandComponent`:
- camera-relative move direction
- move magnitude
- run/jump/attack/interact wishes

### CharacterMovement
Consumes command + motor and updates:
- velocity
- transform
- facing
- derived locomotion metrics

### Combat
Accepts attack / jump requests into gameplay action state.

### Interaction
Accepts interact request into gameplay action state.

### Animation bridge
Pushes locomotion + action state to animation runtime.
Pulls animation notify events back into gameplay notify/action state.

### Graph
Now acts as a thinner orchestration layer:
- grounded -> action
- action -> grounded
- `BeginActionState` enter task

Movement / controller logic is no longer graph-owned.

## Update order

### Pre-animation
1. bootstrap
2. input sources
3. character controller
4. combat requests
5. interaction requests
6. graph execution
7. movement
8. locomotion
9. scene sync
10. animation push

### Post-animation
1. consume animation events
2. mirror gameplay events into graph event queue

## Important note

This is intentionally a **minimal first refactor**:
- `GameplayActionComponent` is still a combined state/request component
- interaction still shares the same action container
- no full ability system yet
- no fully data-driven gameplay configs yet

Those can be added later once the gameplay/domain boundaries are cleaner.

## Editor/Game mode and gameplay camera

A lightweight runtime mode split is now expected by the app layer:

- `GameplayRuntimeMode::Editor`
  - editor gizmos, selection and viewport editing stay enabled
  - gameplay input / movement / transform sync are disabled
- `GameplayRuntimeMode::Game`
  - editor gizmos, selection and viewport interaction are disabled
  - gameplay simulation runs
  - a follow gameplay camera drives the camera basis for movement/controller
  - character facing follows gameplay camera yaw

Current app-level behavior:

- `F5` toggles `Editor <-> Game`
- debug UI exposes the same mode toggle in `App Runtime`
- in `Game` mode the camera follows the controlled gameplay entity and mouse motion updates camera yaw/pitch
- controller reads the updated camera basis and movement keeps the character facing camera yaw
