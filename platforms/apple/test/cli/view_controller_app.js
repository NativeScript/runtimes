class ApplicationDelegate extends NSObject {
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
      NSWindowStyleMask.Resizable |
      NSWindowStyleMask.FullSizeContentView;

    window.titlebarAppearsTransparent = true;
    window.titleVisibility = NSWindowTitleVisibility.Hidden;

    // window.backgroundColor = NSColor.colorWithSRGBRedGreenBlueAlpha(
    //   118 / 255,
    //   171 / 255,
    //   235 / 255,
    //   1,
    // );

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

class ViewController extends NSViewController {
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

  loadView() {
    this.view = NSView.new();
  }

  viewDidLoad() {
    super.viewDidLoad();

    this.view.frame = {
      origin: { x: 0, y: 0 },
      size: { width: 500, height: 500 },
    };

    const label = NSTextField.alloc().initWithFrame({
      origin: { x: 0, y: 0 },
      size: { width: 390, height: 100 },
    });

    label.stringValue = "Hello, macOS";

    label.bezeled = false;
    label.drawsBackground = false;
    label.editable = false;
    label.selectable = false;
    label.alignment = NSTextAlignment.Center;
    label.translatesAutoresizingMaskIntoConstraints = false;
    label.textColor = NSColor.colorWithSRGBRedGreenBlueAlpha(1, 1, 1, 1);
    label.font = NSFontManager.sharedFontManager.convertFontToHaveTrait(
      NSFont.fontWithNameSize(label.font.fontName, 45),
      NSFontTraitMask.Bold,
    );

    label.sizeToFit();

    const vstack = NSStackView.alloc().initWithFrame({
      origin: { x: 0, y: 0 },
      size: { width: 500, height: 500 },
    });

    vstack.orientation = NSUserInterfaceLayoutOrientation.Vertical;
    vstack.alignment = NSLayoutAttribute.CenterX;
    vstack.distribution = NSStackViewDistribution.Fill;
    vstack.spacing = 40;
    vstack.translatesAutoresizingMaskIntoConstraints = false;

    const image = NSImage.alloc().initWithContentsOfFile(
      `${__dirname}/assets/icon.png`,
    );

    if (!image) {
      console.error("Failed to load image");
      return;
    }

    image.size = { width: 128, height: 128 };

    const imageView = NSImageView.imageViewWithImage(image);

    const button = NSButton.buttonWithTitleTargetAction(
      "Try me!",
      this,
      "buttonClicked",
    );
    button.controlSize = NSControlSize.Large;
    button.bezelStyle = NSBezelStyle.Rounded;
    button.setButtonType(NSButtonType.MomentaryLight);
    button.translatesAutoresizingMaskIntoConstraints = false;

    this.button = button;

    const glassView = NSGlassEffectView.alloc().initWithFrame({
      origin: { x: 0, y: 0 },
      size: { width: 300, height: 60 },
    });
    glassView.cornerRadius = 12.0;

    const glassLabel = NSTextField.labelWithString("I'm a glass effect view!");
    glassLabel.translatesAutoresizingMaskIntoConstraints = false;
    glassLabel.alignment = NSTextAlignment.Center;

    // Set the label directly as the contentView
    glassView.contentView = glassLabel;

    // Center the label within the glass effect's contentView
    if (glassView.contentView) {
      glassLabel.centerXAnchor.constraintEqualToAnchor(
        glassView.contentView.centerXAnchor,
      ).active = true;
      glassLabel.centerYAnchor.constraintEqualToAnchor(
        glassView.contentView.centerYAnchor,
      ).active = true;
    }

    vstack.addViewInGravity(imageView, NSStackViewGravity.Center);
    vstack.addViewInGravity(label, NSStackViewGravity.Center);
    vstack.addViewInGravity(button, NSStackViewGravity.Center);
    vstack.addViewInGravity(glassView, NSStackViewGravity.Center);

    this.view.addSubview(vstack);

    vstack.centerXAnchor.constraintEqualToAnchor(
      this.view.centerXAnchor,
    ).active = true;
    vstack.centerYAnchor.constraintEqualToAnchor(
      this.view.centerYAnchor,
    ).active = true;
  }

  /**
   * @param {NSNotification} _sender
   */
  buttonClicked(_sender) {
    if (this.i == 0) {
      const alert = NSAlert.new();
      alert.icon = NSImage.imageWithSystemSymbolNameAccessibilityDescription(
        "cursorarrow.click",
        null,
      );
      alert.messageText = "Clicked for the first time!";
      alert.runModal();
    }

    if (this.button) this.button.title = `Clicked ${++this.i} times`;
  }
}

const NSApp = NSApplication.sharedApplication;

NSApp.delegate = ApplicationDelegate.new();

NSApp.setActivationPolicy(NSApplicationActivationPolicy.Regular);

NSApplicationMain(0, null);
