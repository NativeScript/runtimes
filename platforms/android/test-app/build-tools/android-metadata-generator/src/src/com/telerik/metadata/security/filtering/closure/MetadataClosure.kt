package com.telerik.metadata.security.filtering.closure

import com.telerik.metadata.ClassMapProvider
import com.telerik.metadata.parsing.NativeClassDescriptor
import com.telerik.metadata.security.filtering.MetadataFilter
import com.telerik.metadata.storage.functions.extensions.ExtensionFunctionsStorage

/**
 * Expands a seed set of class names into one the metadata tree can actually be
 * built from.
 *
 * A whitelist naming exactly the classes an app was observed to use does not
 * work: it produces metadata that cannot start the app. Two reasons, both
 * measured -- see docs/metadata-filtering.md.
 *
 *  - A retained class's *supertypes* carry most of its members.
 *  - A retained method's *signature types* must be present **exactly**. When one
 *    is missing, Builder.getOrCreateNode does not fail; it substitutes the
 *    nearest retained ancestor via findNearestAllowedClass. That silently
 *    rewrites `onCreate(Bundle)` to `onCreate(Object)`, which then fails to
 *    resolve against the real method at runtime:
 *
 *        java.lang.NoSuchMethodError: no non-static method
 *            "Landroid/app/Activity;.onCreate(Ljava/lang/Object;)V"
 *
 * So the closure's job is to make widening never happen for anything reachable.
 *
 * [signatureDepth] bounds how far signature types are chased. Depth 1 keeps the
 * signatures of the seed exact; the types pulled in at the last level may still
 * have their own signatures widened, which only matters if the app calls into
 * them. Unbounded is the safe end and the default -- it converged well short of
 * the whole classpath on the test-app, so the size win survives it.
 */
class MetadataClosure(
        private val providers: List<ClassMapProvider>,
        private val blacklist: MetadataFilter,
        private val signatureDepth: Int = Int.MAX_VALUE
) {

    /** Reported so the build can print what the closure cost. */
    var seedSize: Int = 0
        private set

    private val byName = HashMap<String, NativeClassDescriptor>()

    /** outer class name -> its directly nested classes */
    private val nestedByOuter = HashMap<String, MutableList<String>>()

    private fun index() {
        if (byName.isNotEmpty()) return

        for (provider in providers) {
            for ((name, descriptor) in provider.classMap) {
                byName.putIfAbsent(name, descriptor)

                val idx = name.lastIndexOf('$')
                if (idx > 0) {
                    nestedByOuter.getOrPut(name.substring(0, idx)) { ArrayList() }.add(name)
                }
            }
        }
    }

    /**
     * The element type of an arbitrarily nested array signature, or null when
     * the signature bottoms out in a primitive.
     */
    private fun elementClassName(signature: String): String? {
        var s = signature
        while (s.startsWith("[")) {
            s = s.substring(1)
        }
        if (!s.startsWith("L") || !s.endsWith(";")) {
            return null
        }
        return s.substring(1, s.length - 1).replace('/', '.')
    }

    private fun isBlacklisted(descriptor: NativeClassDescriptor): Boolean {
        val simple = descriptor.className
                .substring(descriptor.packageName.length)
                .replace('$', '.')
                .trimStart('.')
        return !blacklist.isAllowed(descriptor.packageName, simple).isAllowed
    }

    /**
     * @param isSeed decides membership of the starting set, by (package, class)
     *               exactly as the whitelist does.
     * @return every class name whose metadata must be emitted.
     */
    fun compute(isSeed: (NativeClassDescriptor) -> Boolean): Set<String> {
        index()

        val retained = HashSet<String>()
        /* Worklist entries carry the remaining signature budget, so a class
         * reached at depth 0 still contributes its supertypes -- those are
         * unconditional -- but stops pulling in new signature types. */
        val pending = ArrayDeque<Pair<String, Int>>()

        fun enqueue(name: String, budget: Int) {
            val descriptor = byName[name] ?: return
            if (isBlacklisted(descriptor)) return
            if (!retained.add(name)) return
            pending.addLast(name to budget)
        }

        for ((name, descriptor) in byName) {
            if (isSeed(descriptor) && !isBlacklisted(descriptor)) {
                if (retained.add(name)) {
                    pending.addLast(name to signatureDepth)
                }
            }
        }
        seedSize = retained.size

        while (pending.isNotEmpty()) {
            val (name, budget) = pending.removeFirst()
            val descriptor = byName[name] ?: continue

            /* Supertypes and interfaces: unconditional. A class without them
             * loses the members it inherits, and findNearestAllowedClass would
             * silently rebase it on something further up. */
            descriptor.superclassName
                    .takeIf { it.isNotEmpty() }
                    ?.let { enqueue(it, budget) }

            for (interfaceName in descriptor.interfaceNames) {
                enqueue(interfaceName, budget)
            }

            /* The enclosing class, so a nested class is reachable at all, and
             * the nested classes, which JS names through their outer. */
            val idx = name.lastIndexOf('$')
            if (idx > 0) {
                enqueue(name.substring(0, idx), budget)
            }
            for (nested in nestedByOuter[name].orEmpty()) {
                enqueue(nested, budget)
            }

            /* Kotlin extension functions appear on the receiver but are
             * declared in a file class the app never names -- there is no
             * reference anywhere to pull it in, so it has to be looked up from
             * the receiver side. Unconditional: dropping it silently removes
             * methods from a class that is being kept. */
            for (extension in ExtensionFunctionsStorage.getInstance().retrieveFunctions(name)) {
                extension.declaringClass.className
                        .takeIf { it.isNotEmpty() }
                        ?.let { enqueue(it, budget) }
            }

            if (budget <= 0) continue
            val next = if (budget == Int.MAX_VALUE) budget else budget - 1

            /* Signature types: the step whose absence crashes the app at
             * launch rather than at the call site. */
            for (method in descriptor.methods) {
                elementClassName(method.returnType.signature)?.let { enqueue(it, next) }
                for (argument in method.argumentTypes) {
                    elementClassName(argument.signature)?.let { enqueue(it, next) }
                }
            }

            for (field in descriptor.fields) {
                elementClassName(field.type.signature)?.let { enqueue(it, next) }
            }
        }

        return retained
    }
}
