import * as path from 'path';

/** @brief Supplies the host environment and executable file check for runner discovery. */
export interface RunnerResolverDependencies {
    /** @brief Environment variables to search for PATH and, on Windows, PATHEXT. */
    readonly env: Readonly<NodeJS.ProcessEnv>;
    /** @brief Platform whose path syntax and executable extensions apply. */
    readonly platform: NodeJS.Platform;
    /** @brief Checks whether a candidate is an executable file. */
    readonly fileExists: (candidate: string) => boolean;
}

/**
 * @brief Finds the external AngelScript runner from an explicit setting or PATH.
 * @param[in] setting Absolute runner path, or an empty string to search PATH.
 * @param[in] deps Host discovery inputs, including the candidate file check.
 * @return The first matching path, or a not-found error if discovery fails.
 */
export function resolveRunner(
    setting: string,
    { env, platform, fileExists }: RunnerResolverDependencies
): { path: string } | { error: 'not-found' } {
    const windows = platform === 'win32';
    const platformPath = windows ? path.win32 : path.posix;
    if (setting) {
        return platformPath.isAbsolute(setting) && fileExists(setting)
            ? { path: setting }
            : { error: 'not-found' };
    }

    const readEnv = (name: string): string | undefined => {
        const key = windows ? Object.keys(env).find(key => key.toUpperCase() === name) : name;
        return key === undefined ? undefined : env[key];
    };
    const extensions = windows
        ? (readEnv('PATHEXT') || '.EXE').split(platformPath.delimiter)
            .map(extension => extension.trim().toLowerCase())
            .filter(Boolean)
        : [''];
    for (const entry of (readEnv('PATH') ?? '').split(platformPath.delimiter)) {
        const directory = windows ? entry.replace(/^"(.*)"$/, '$1') : entry;
        if (!directory) {
            continue;
        }
        for (const extension of extensions) {
            const candidate = platformPath.join(directory, `angelscript-cli${extension}`);
            if (fileExists(candidate)) {
                return { path: candidate };
            }
        }
    }
    return { error: 'not-found' };
}
