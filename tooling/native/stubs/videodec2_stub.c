/*
 * Mediathek - Link-time names of the system's libSceVideodec2.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The SDK has no stub for this library. This becomes one (tools/build.sh): the
 * bodies are never packaged or run, the native module writer makes system imports of
 * these names. List only what the app calls: the loader has to find each of them on
 * the console, and refuses to start the app if it cannot.
 */

void sceVideodec2QueryComputeMemoryInfo(void)
{
}
void sceVideodec2AllocateComputeQueue(void)
{
}
void sceVideodec2ReleaseComputeQueue(void)
{
}
void sceVideodec2QueryDecoderMemoryInfo(void)
{
}
void sceVideodec2CreateDecoder(void)
{
}
void sceVideodec2DeleteDecoder(void)
{
}
void sceVideodec2Reset(void)
{
}
void sceVideodec2Decode(void)
{
}
void sceVideodec2Flush(void)
{
}
