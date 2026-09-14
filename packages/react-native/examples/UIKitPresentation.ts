import NativeScript from '@nativescript/react-native';

export function topVisibleViewController(
  root: UIViewController | null | undefined =
    UIApplication.sharedApplication.keyWindow?.rootViewController,
): UIViewController | null {
  'worklet';
  let current = root ?? null;
  while (current?.presentedViewController) {
    current = current.presentedViewController;
  }
  const selected = (current as UITabBarController | null)?.selectedViewController;
  if (selected) {
    return topVisibleViewController(selected);
  }
  const visible = (current as UINavigationController | null)?.visibleViewController;
  if (visible) {
    return topVisibleViewController(visible);
  }
  return current;
}

export async function presentDocumentCamera(
  delegate: VNDocumentCameraViewControllerDelegate,
) {
  await NativeScript.scheduleOnUI(() => {
    'worklet';
    if (
      !NativeScript.loadFramework('VisionKit') ||
      !NativeScript.isClassAvailable('VNDocumentCameraViewController')
    ) {
      throw new Error('VisionKit document camera is not available');
    }

    const CameraController =
      NativeScript.getClass<typeof VNDocumentCameraViewController>(
        'VNDocumentCameraViewController',
      );
    const presenter = topVisibleViewController();
    if (!CameraController || !presenter || presenter.presentedViewController) {
      return;
    }

    const controller = CameraController.new();
    controller.delegate = delegate;
    presenter.presentViewControllerAnimatedCompletion(controller, true, null);
  });
}

export async function presentPasses(pass: PKPass) {
  await NativeScript.scheduleOnUI(() => {
    'worklet';
    if (
      !NativeScript.loadFramework('PassKit') ||
      !NativeScript.isClassAvailable('PKAddPassesViewController')
    ) {
      throw new Error('PassKit add-passes UI is not available');
    }

    const AddPassesController =
      NativeScript.getClass<typeof PKAddPassesViewController>(
        'PKAddPassesViewController',
      );
    const presenter = topVisibleViewController();
    if (!AddPassesController || !presenter || presenter.presentedViewController) {
      return;
    }

    const controller = AddPassesController.alloc().initWithPass(pass);
    presenter.presentViewControllerAnimatedCompletion(controller, true, null);
  });
}
