package com.telerik.metadata.security.classes

import com.telerik.metadata.ClassMapProvider
import com.telerik.metadata.parsing.NativeClassDescriptor
import com.telerik.metadata.security.filtering.blacklisting.MetadataBlacklist
import com.telerik.metadata.security.filtering.input.user.UserPatternsCollection
import com.telerik.metadata.security.filtering.matching.impl.PatternMatcherImpl
import com.telerik.metadata.security.filtering.whitelisting.MetadataWhitelist
import com.telerik.metadata.security.logging.console.MetadataFilterConsoleLogger
import java.util.*
import kotlin.collections.HashSet

object SecuredClassRepository {
    private val cachedProviders = ArrayList<ClassMapProvider>()
    private val patternMatcher = PatternMatcherImpl()
    private val whitelist = MetadataWhitelist(UserPatternsCollection.whitelistEntries, patternMatcher)
    private val blacklist = MetadataBlacklist(UserPatternsCollection.blacklistEntries, patternMatcher)
    private val alreadyLoggedClasses = HashSet<Pair<String, String>>()

    fun addToCache(classMapProvider: ClassMapProvider) {
        cachedProviders.add(classMapProvider)
    }

    private fun NativeClassDescriptor.getSimplifiedClassName() = this.className.substring(this.packageName.length).replace('$', '.').trimStart('.')

    fun findClass(className: String): SecuredNativeClassDescriptor {
        val clazz: NativeClassDescriptor? = findClassFromProviders(className)

        if (clazz != null) {
            val simplifiedClassName = clazz.getSimplifiedClassName()

            val isUsageAllowed = isClassUsageAllowed(clazz.packageName, simplifiedClassName)
            if (isUsageAllowed) {
                return SecuredNativeClassDescriptor(true, clazz)
            }
        }

        return SecuredNativeClassDescriptor(false, NativeClassDescriptor.Missing)
    }

    fun findNearestAllowedClass(className: String): SecuredNativeClassDescriptor {
        var clazz: NativeClassDescriptor? = findClassFromProviders(className)

        while (clazz != null) {
            val simplifiedClassName = clazz.getSimplifiedClassName()

            val isUsageAllowed = isClassUsageAllowed(clazz.packageName, simplifiedClassName)
            if (isUsageAllowed) {
                return SecuredNativeClassDescriptor(true, clazz)
            }

            clazz = findClassFromProviders(clazz.superclassName)
        }

        return SecuredNativeClassDescriptor(false, NativeClassDescriptor.Missing)
    }

    /**
     * When a closure has been computed, it -- not the raw whitelist -- decides
     * what is emitted. The whitelist is only the seed it grew from; a class the
     * closure added is required for the tree to be internally consistent, so
     * consulting the patterns again here would drop it right back out.
     * Null means no filtering was requested and everything is allowed.
     */
    private var retainedClasses: Set<String>? = null

    fun applyClosure(retained: Set<String>) {
        retainedClasses = retained
    }

    fun getRetainedClasses(): Set<String>? = retainedClasses

    fun getCachedProviders(): List<ClassMapProvider> = cachedProviders

    /** Bypasses filtering; for the passes that must run before the filter exists. */
    fun findClassUnfiltered(className: String): NativeClassDescriptor? =
            findClassFromProviders(className)

    private fun isClassUsageAllowed(packageName: String, className: String): Boolean {
        val retained = retainedClasses
        if (retained != null) {
            /* className arrives simplified ($ -> .); the closure is keyed by the
             * binary name, which is what the class map uses. */
            val binaryName = if (packageName.isEmpty()) {
                className.replace('.', '$')
            } else {
                packageName + "." + className.replace('.', '$')
            }
            return binaryName in retained
        }

        val whitelistFilterResult = whitelist.isAllowed(packageName, className)
        val blacklistFilterResult = blacklist.isAllowed(packageName, className)

        val packageAndClassNamePair = packageName to className

        if (whitelistFilterResult.isAllowed && blacklistFilterResult.isAllowed) {
            if (packageAndClassNamePair !in alreadyLoggedClasses) {
                MetadataFilterConsoleLogger.logAllowed(packageName, className, whitelistFilterResult, blacklistFilterResult)
                alreadyLoggedClasses.add(packageAndClassNamePair)
            }

            return true
        }

        if (packageAndClassNamePair !in alreadyLoggedClasses) {
            MetadataFilterConsoleLogger.logDisallowed(packageName, className, whitelistFilterResult, blacklistFilterResult)
            alreadyLoggedClasses.add(packageAndClassNamePair)
        }

        return false
    }

    private fun findClassFromProviders(className: String): NativeClassDescriptor? {
        var clazz: NativeClassDescriptor? = null
        for (classMapProvider in cachedProviders) {
            clazz = classMapProvider.classMap[className]
            if (clazz != null) {
                break
            }
        }

        return clazz
    }

    fun getClassNames(): Array<String> {
        val names = ArrayList<String>()
        for (classMapProvider in cachedProviders) {
            for (className in classMapProvider.classMap.keys) {
                names.add(className)
            }
        }
        val arrClassNames = names.toTypedArray()
        Arrays.sort(arrClassNames)
        return arrClassNames
    }
}