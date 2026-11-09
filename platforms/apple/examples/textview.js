// @ts-check

import "@nativescript/macos-node-api";

export class ApplicationDelegate extends NSObject {
  static ObjCProtocols = [NSApplicationDelegate, NSWindowDelegate];

  static {
    NativeClass(this);
  }

  /**
   * @param {NSNotification} _notification
   */
  applicationDidFinishLaunching(_notification) {
    const menu = NSMenu.new();
    NSApp.mainMenu = menu;

    const appMenuItem = NSMenuItem.new();
    menu.addItem(appMenuItem);

    const appMenu = NSMenu.new();
    appMenuItem.submenu = appMenu;

    appMenu.addItemWithTitleActionKeyEquivalent("Quit", "terminate:", "q");

    const controller = ViewController.new();
    const window = NSWindow.windowWithContentViewController(controller);

    window.title = "NativeScript for macOS";
    window.delegate = this;
    window.styleMask =
      NSWindowStyleMask.Titled |
      NSWindowStyleMask.Closable |
      NSWindowStyleMask.Miniaturizable |
      NSWindowStyleMask.Resizable;

    window.makeKeyAndOrderFront(this);

    NSApp.activateIgnoringOtherApps(false);
  }

  /**
   * @param {NSNotification} _notification
   */
  windowWillClose(_notification) {
    NSApp.terminate(this);
  }
}

export class ViewController extends NSViewController {
  static ObjCExposedMethods = {
    buttonClicked: {
      params: [NSView],
      returns: interop.types.void,
    },
  };

  static {
    NativeClass(this);
  }

  i = 0;
  /**
   * @type {NSButton | null}
   */
  button = null;

  /** @override */
  loadView() {
    this.view = NSView.new();
  }

  /** @override */
  viewDidLoad() {
    super.viewDidLoad();

    this.view.frame = {
      origin: { x: 0, y: 0 },
      size: { width: 500, height: 500 },
    };

    const textView = NSTextView.new();

    textView.string = "This is a NSTextView.\nYou can edit this text.";
    textView.font = NSFont.systemFontOfSize(16);
    textView.editable = true;

    textView.translatesAutoresizingMaskIntoConstraints = false;
    textView.frame = this.view.bounds;
    textView.autoresizingMask =
      NSAutoresizingMaskOptions.WidthSizable |
      NSAutoresizingMaskOptions.HeightSizable;

    this.view.addSubview(textView);
  }
}

const NSApp = NSApplication.sharedApplication;

NSApp.delegate = ApplicationDelegate.new();

NSApp.setActivationPolicy(NSApplicationActivationPolicy.Regular);

NSApplicationMain(0, null);
