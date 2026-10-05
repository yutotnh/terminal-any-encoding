/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Microsoft Corporation. All rights reserved.
 *  Licensed under the MIT License. See License.txt in the project root for license information.
 *--------------------------------------------------------------------------------------------*/
// Test-only proposed API type definitions. Not used by product code.
// Source: https://github.com/microsoft/vscode/blob/1.135.0/src/vscode-dts/vscode.proposed.terminalDataWriteEvent.d.ts

declare module "vscode" {
  export interface TerminalDataWriteEvent {
    readonly terminal: Terminal;
    readonly data: string;
  }

  namespace window {
    export const onDidWriteTerminalData: Event<TerminalDataWriteEvent>;
  }
}
