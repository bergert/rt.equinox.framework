/*******************************************************************************
 * Copyright (c) 2006, 2015 IBM Corporation and others.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * which accompanies this distribution, and is available at
 * http://www.eclipse.org/legal/epl-v10.html
 *
 * Contributors:
 *     IBM Corporation - initial API and implementation
 *     Andrew Niefer
 *     Mikael Barbero
 * Contributors:
 *     Bug 494293 [Mac] fixed configuration folder; updated for 47
 *******************************************************************************/

#include "eclipseCommon.h"
#include "eclipseOS.h"

#include <locale.h>
#include <dlfcn.h>
#include <unistd.h>
#include <CoreServices/CoreServices.h>
#include <Cocoa/Cocoa.h>
#include <mach-o/dyld.h>

char   dirSeparator  = '/';
char   pathSeparator = ':';

static CFBundleRef javaVMBundle = NULL;

int initialized = 0;

static void init() {
	if (!initialized) {
		[[NSApplication sharedApplication] setActivationPolicy: NSApplicationActivationPolicyRegular];
		[[NSRunningApplication currentApplication] activateWithOptions: NSApplicationActivateIgnoringOtherApps];
		initialized= true;
	}
}


/* Initialize Window System
 *
 * Initialize Cocoa.
 */
int initWindowSystem( int* pArgc, char* argv[], int showSplash )
{
	char *homeDir = getProgramDir();
	/*debug("install dir: %s\n", homeDir);*/
	if (homeDir != NULL)
		chdir(homeDir);

	if (showSplash)
		init();

	return 0;
}

/* Display a Message */
void displayMessage(char *title, char *message)
{
	CFStringRef inError, inDescription= NULL;

	/* try to break the message into a first sentence and the rest */
	char *pos= strstr(message, ". ");
	if (pos != NULL) {
		char *to, *from, *buffer= calloc(pos-message+2, sizeof(char));
		/* copy and replace line separators with blanks */
		for (to= buffer, from= message; from <= pos; from++, to++) {
			char c= *from;
			if (c == '\n') c= ' ';
			*to= c;
		}
		inError= CFStringCreateWithCString(kCFAllocatorDefault, buffer, kCFStringEncodingUTF8);
		free(buffer);
		inDescription= CFStringCreateWithCString(kCFAllocatorDefault, pos+2, kCFStringEncodingUTF8);
	} else {
		inError= CFStringCreateWithCString(kCFAllocatorDefault, message, kCFStringEncodingUTF8);
	}

	init();

	NSAutoreleasePool* pool = [[NSAutoreleasePool alloc] init];
	NSAlert* alert = [NSAlert alertWithMessageText: (NSString*)(inDescription != nil ? inError : nil) defaultButton: nil alternateButton: nil otherButton: nil informativeTextWithFormat: (NSString*)(inDescription != nil ? inDescription : inError)];
	[[alert window] setTitle: [NSString stringWithUTF8String: title]];
	[alert setAlertStyle: NSCriticalAlertStyle];
	[alert runModal];
	[pool release];
	CFRelease(inError);
	if (inDescription != NULL)
		CFRelease(inDescription);
}

static int isLibrary( _TCHAR* vm ){
	_TCHAR *ch = NULL;
	if (vm == NULL) return 0;
	ch = _tcsrchr( vm, '.' );
	if(ch == NULL)
		return 0;
	return (_tcsicmp(ch, _T_ECLIPSE(".so")) == 0) || (_tcsicmp(ch, _T_ECLIPSE(".jnilib")) == 0) || (_tcsicmp(ch, _T_ECLIPSE(".dylib")) == 0);
}

static void loadVMBundle( char * bundle ) {
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, (const UInt8 *)bundle, strlen(bundle), true);
	javaVMBundle = CFBundleCreate(kCFAllocatorDefault, url);
	CFRelease(url);
}

/* Load the specified shared library
 */
void * loadLibrary( char * library ){
	if (!isLibrary(library)) {
		loadVMBundle(library);
		return (void*) &javaVMBundle;
	}

	_TCHAR *bundle = strdup(library), *start;

	// check if it's a JVM bundle
	if (strstr(bundle, "libjvm") && (start = strstr(bundle, "/Contents/Home/")) != NULL) {
		start[0] = NULL;
		loadVMBundle(bundle);
		free(bundle);
		if (javaVMBundle) {
			return (void*) &javaVMBundle;
		}
	}

	free(bundle);
	void * result= dlopen(library, RTLD_NOW);
	if(result == 0)
		printf("%s\n",dlerror());
	return result;
}

/* Unload the shared library
 */
void unloadLibrary( void * handle ){
	if (handle == &javaVMBundle)
		CFRelease(javaVMBundle);
	else
		dlclose(handle);
}

/* Find the given symbol in the shared library
 */
void * findSymbol( void * handle, char * symbol ){
	if(handle == &javaVMBundle) {
		CFStringRef string = CFStringCreateWithCString(kCFAllocatorDefault, symbol, kCFStringEncodingASCII);
		void * ptr = CFBundleGetFunctionPointerForName(javaVMBundle, string);
		CFRelease(string);
		return ptr;
	} else
		return dlsym(handle, symbol);
}

char * resolveSymlinks( char * path ) {
	char * result = 0;
	CFURLRef url, resolved;
	CFStringRef string;
	FSRef fsRef;
	Boolean isFolder, wasAliased;

	if(path == NULL)
		return path;

	string = CFStringCreateWithCString(kCFAllocatorDefault, path, kCFStringEncodingUTF8);
	url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, string, kCFURLPOSIXPathStyle, false);
	CFRelease(string);
	if(url == NULL)
		return path;

	if(CFURLGetFSRef(url, &fsRef)) {
		if( FSResolveAliasFile(&fsRef, true, &isFolder, &wasAliased) == noErr) {
			resolved = CFURLCreateFromFSRef(kCFAllocatorDefault, &fsRef);
			if(resolved != NULL) {
				string = CFURLCopyFileSystemPath(resolved, kCFURLPOSIXPathStyle);
				CFIndex length = CFStringGetMaximumSizeForEncoding(CFStringGetLength(string), kCFStringEncodingUTF8);
				char *s = malloc(length);
				if (CFStringGetCString(string, s, length, kCFStringEncodingUTF8)) {
					result = s;
				} else {
					free(s);
				}
				CFRelease(string);
				CFRelease(resolved);
			}
		}
	}
	CFRelease(url);
	return result;
}

/*
 * copies the config file from MyApp.app/Contents/Resources/configuraion folder to the specified folder inside user home directory
 * programdir: the path where the program runs ("MyApp.app/Contents/MacOS")
 * relpath: the relative path to the configuraion folder ("../Resources/configuraion")
 * destpath: the destination user-path for the configuraion ("~/fishstatj_workspace/configuration")
 */
int copyConfigFile(_TCHAR* programdir, _TCHAR* relpath, _TCHAR* destpath) {

	// append (programdir, relpath) and convert relpath to an absolute path (stringByStandardizingPath)
	NSString *sourcePath        = [[[NSString stringWithUTF8String: programdir] stringByAppendingPathComponent:[NSString stringWithUTF8String: relpath]] stringByStandardizingPath];
	// expand the tilde home directory
	NSString *configurationFolder = [[NSString stringWithUTF8String: destpath] stringByExpandingTildeInPath];

    NSFileManager * fileManager = [ NSFileManager defaultManager];
	printf("programdir =  %s\n", programdir);
	printf("sourcePath =  %s\n", [sourcePath UTF8String]);

    // remove configuration folder
    NSError *error = nil;
    if (! [fileManager removeItemAtPath:configurationFolder error:&error]) {
    	printf("Could not delete path %s. error", [configurationFolder UTF8String]);
    }
	printf("OK removed %s\n", [configurationFolder UTF8String]);

    // remove .metadata folder
    error = nil;
    NSString *metadataPath = [[configurationFolder stringByDeletingLastPathComponent] stringByAppendingPathComponent:@".metadata"];
	printf("removed %s\n", [metadataPath UTF8String]);
    [fileManager removeItemAtPath:metadataPath error:&error];

    //finally, copy the whole configuration folder
    error = nil;
    if ( !( [ fileManager copyItemAtPath:sourcePath toPath:configurationFolder error:&error ]) )  {
    	printf("Could not copy file at path %s to path %s. error",[sourcePath UTF8String], [configurationFolder UTF8String]);
    	NSLog(@"Error: %@", error);
        [error release];
        return NO;
    }
    return YES;
}
