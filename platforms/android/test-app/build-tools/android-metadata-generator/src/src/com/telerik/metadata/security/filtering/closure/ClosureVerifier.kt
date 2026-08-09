package com.telerik.metadata.security.filtering.closure

/**
 * Independent check that the filter did not quietly change the meaning of any
 * signature it emitted.
 *
 * The closure is supposed to retain every type a retained signature mentions.
 * If it misses one, nothing fails at build time: Builder.getOrCreateNode calls
 * findNearestAllowedClass, which substitutes the nearest retained *ancestor*.
 * `onCreate(Bundle)` becomes `onCreate(Object)`, the metadata is written
 * happily, and the app dies at the call site:
 *
 *     java.lang.NoSuchMethodError: no non-static method
 *         "Landroid/app/Activity;.onCreate(Ljava/lang/Object;)V"
 *
 * So the substitution *is* the defect, and this records every one. The check
 * does not re-derive the closure -- it watches what generation actually did,
 * which is why it catches a closure that is wrong as well as one that is
 * incomplete.
 *
 * Only substitutions of types that exist on the classpath count. A type that is
 * genuinely absent is widened unfiltered too; that is pre-existing behaviour and
 * an app can compile against an SDK it does not ship.
 */
object ClosureVerifier {

    data class Widening(val requested: String, val substituted: String, val context: String)

    private val widenings = LinkedHashMap<String, Widening>()

    var enabled: Boolean = false

    /** Set while a class's members are generated, to attribute the violation. */
    var currentContext: String = ""

    fun record(requested: String, substituted: String) {
        if (!enabled) return
        if (requested == substituted) return

        widenings.putIfAbsent(requested, Widening(requested, substituted, currentContext))
    }

    fun violations(): Collection<Widening> = widenings.values

    fun reset() {
        widenings.clear()
        currentContext = ""
    }
}
