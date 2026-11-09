const nsStr = NSString.stringWithString("0123");

try {
  console.log(nsStr.substringFromIndex(5));
} catch (e) {
  console.log(e.stack);
}

// NSOperationQueue.mainQueue.addOperationWithBlock(() => {
//   console.log("Hello, World! from main queue");

//   throw new Error("JavaScript error from block");
// });

// NSTimer.scheduledTimerWithTimeIntervalRepeatsBlock(
//   1,
//   false,
//   () => {
//     console.log("Hello, World! from timer");
//     throw new Error("JavaScript error from timer");
//   },
// );

// globalThis.__onUncaughtError = (err) => {
//   console.log("Hello, World! from globalThis.__onUncaughtError");
//   console.log(err.stack);
// };

// class ApplicationDelegate extends NSObject {
//   static ObjCProtocols = [NSApplicationDelegate];

//   static {
//     NativeClass(this);
//   }

//   applicationDidFinishLaunching(_notification) {
//     console.log("Hello, World! from applicationDidFinishLaunching");
//     throw new Error("JavaScript error from applicationDidFinishLaunching");
//   }
// }

// const appDelegate = ApplicationDelegate.new();
// const app = NSApplication.sharedApplication;
// app.delegate = appDelegate;
// app.setActivationPolicy(NSApplicationActivationPolicy.Regular);

// NSApplicationMain(0, null);

const arr = NSArray.arrayWithArray([1, 2, 3]);

try {
  arr.enumerateObjectsUsingBlock((_obj, _idx, _stop) => {
    throw new Error("JavaScript error from block");
  });
} catch (e) {
  console.log(e.stack);
}
