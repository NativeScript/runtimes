// NSOperationQueue.mainQueue.addOperationWithBlock(() => {
//   console.log("Hello from the main queue");
// });

// const bgQueue = NSOperationQueue.new();

// bgQueue.addOperationWithBlock(() => {
//   console.log("Hello from the background queue");
// });

dispatch_async(dispatch_get_current_queue(), () => {
  console.log("Hello from the main queue");
});

dispatch_async(dispatch_get_global_queue(qos_class_t.DEFAULT, 0), () => {
  console.log("Hello from the background queue");
});

NSApplicationMain(0, null);
