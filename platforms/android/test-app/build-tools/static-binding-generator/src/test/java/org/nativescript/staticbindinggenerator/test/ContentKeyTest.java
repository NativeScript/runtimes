package org.nativescript.staticbindinggenerator.test;

import org.junit.Test;
import org.nativescript.staticbindinggenerator.ContentKey;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotEquals;

/**
 * Pins the generator's half of the content key against values produced by the
 * runtime's MetadataNode::CreateContentKey.
 *
 * The two implementations are in different languages and cannot call each
 * other, so the literals below are the contract between them. If one side is
 * changed without the other, this test fails here rather than as a missing
 * class on device -- which is the only other place the disagreement shows up.
 */
public class ContentKeyTest {

    @Test
    public void matchesRuntimeForOverridesOnly() {
        assertEquals("h4b3d4e7b5dcd7eac",
                ContentKey.of("com.tns.tests.Button1", new String[]{""}, new String[]{"toString", "echo"}));
    }

    @Test
    public void matchesRuntimeForInterfaces() {
        assertEquals("h43486683deee2d72",
                ContentKey.of("java.lang.Object",
                        new String[]{"java.lang.Runnable", "java.io.Closeable"},
                        new String[]{"run", "close"}));
    }

    @Test
    public void matchesRuntimeForSingleOverride() {
        assertEquals("he2246413e8de25b9",
                ContentKey.of("android.view.View", new String[]{""}, new String[]{"onClick"}));
    }

    /** The runtime's base class name is slash-separated; both must canonicalize. */
    @Test
    public void treatsSlashAndDotBasesAlike() {
        assertEquals(ContentKey.of("com.tns.tests.Button1", new String[]{""}, new String[]{""}),
                ContentKey.of("com/tns/tests/Button1", new String[]{""}, new String[]{""}));
        assertEquals("h55d3a3a72d6a9f05",
                ContentKey.of("com/tns/tests/Button1", new String[]{""}, new String[]{""}));
    }

    /**
     * A nested interface is `Pack200$Unpacker` to the runtime and
     * `Pack200.Unpacker` in JS source. Both sides collapse '$' to '.', so the
     * two spellings have to hash alike -- before this held, every class
     * implementing a nested interface was built on device instead.
     *
     * The literal is the real key for the test-app's
     * MyTSTranspiledObjectThatExtendsInterfaces.
     */
    @Test
    public void treatsNestedAndDottedInterfacesAlike() {
        String[] dotted = {"java.util.jar.Pack200.Unpacker", "java.util.Formattable",
                "java.util.Observer", "java.util.jar.Pack200.Packer"};
        String[] binary = {"java/util/jar/Pack200$Unpacker", "java.util.Formattable",
                "java.util.Observer", "java/util/jar/Pack200$Packer"};
        String[] methods = {"greet", "unpack", "properties", "toString", "formatTo", "update",
                "addPropertyChangeListener", "pack", "removePropertyChangeListener"};

        assertEquals(ContentKey.of("java.lang.Object", dotted, methods),
                ContentKey.of("java.lang.Object", binary, methods));
        assertEquals("haa054d88a5def37b",
                ContentKey.of("java.lang.Object", dotted, methods));
    }

    /** The same collapse applies to a nested base class. */
    @Test
    public void treatsNestedAndDottedBasesAlike() {
        assertEquals(
                ContentKey.of("com.tns.tests.DummyClass.DummyDerivedClass", new String[]{""},
                        new String[]{"toString"}),
                ContentKey.of("com/tns/tests/DummyClass$DummyDerivedClass", new String[]{""},
                        new String[]{"toString"}));
    }

    /**
     * An absent field in sbg-bindings.txt splits to one empty string. The runtime
     * has no such element, so it has to be dropped rather than hashed as "".
     */
    @Test
    public void ignoresTheEmptyFieldPlaceholder() {
        assertEquals(ContentKey.of("android.view.View", new String[0], new String[]{"onClick"}),
                ContentKey.of("android.view.View", new String[]{""}, new String[]{"onClick"}));
    }

    /** Order is the one thing the two sides provably disagree on, so it must not count. */
    @Test
    public void isOrderIndependent() {
        assertEquals(ContentKey.of("android.view.View", new String[]{"a.B", "c.D"}, new String[]{"x", "y"}),
                ContentKey.of("android.view.View", new String[]{"c.D", "a.B"}, new String[]{"y", "x"}));
    }

    /** Everything that reaches the generated Java must still separate classes. */
    @Test
    public void separatesDifferentContent() {
        String base = ContentKey.of("android.view.View", new String[0], new String[]{"onClick"});
        assertNotEquals(base, ContentKey.of("android.view.ViewGroup", new String[0], new String[]{"onClick"}));
        assertNotEquals(base, ContentKey.of("android.view.View", new String[0], new String[]{"onTouch"}));
        assertNotEquals(base, ContentKey.of("android.view.View", new String[]{"a.B"}, new String[]{"onClick"}));
    }

    /** The delimiters must not be forgeable from a name that contains them. */
    @Test
    public void doesNotConfuseFieldBoundaries() {
        assertNotEquals(ContentKey.of("android.view.View", new String[]{"a.B"}, new String[0]),
                ContentKey.of("android.view.View", new String[0], new String[]{"a.B"}));
    }
}
