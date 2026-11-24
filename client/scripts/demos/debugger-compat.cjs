const path = require('node:path');
const vscode = require('vscode');

exports.activate = context => {
  context.subscriptions.push(vscode.debug.registerDebugAdapterTrackerFactory('angelscript', {
    createDebugAdapterTracker(session) {
      if (session.configuration.name !== 'Attach to AngelScript Demo') return;

      return {
        onDidSendMessage(message) {
          if (message.type !== 'response' || !message.body) return;

          // The example host advertises this capability without a request handler.
          // VS Code waits for the empty instruction-breakpoint request before configurationDone.
          if (message.command === 'initialize') {
            message.body.supportsInstructionBreakpoints = false;
          }
          if (message.command === 'stackTrace') {
            for (const frame of message.body.stackFrames ?? []) {
              if (frame.source?.path) frame.source.name = path.basename(frame.source.path);
            }
          }
        },
      };
    },
  }));
};
