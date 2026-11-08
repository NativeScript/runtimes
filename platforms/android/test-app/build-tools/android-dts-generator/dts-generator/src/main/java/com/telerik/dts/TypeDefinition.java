package com.telerik.dts;

import org.apache.bcel.generic.ObjectType;
import org.apache.bcel.generic.ReferenceType;
import org.apache.bcel.generic.Type;

import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import edu.umd.cs.findbugs.ba.generic.GenericObjectType;
import edu.umd.cs.findbugs.ba.generic.GenericUtilities;

public class TypeDefinition {
    private static Pattern typeSignature = Pattern.compile("^(<(?<TypeGenerics>.*)>)?(?<Interfaces>L.*)$");
    //private static Pattern extendsGenericType = Pattern.compile("(?<GenericLetter>\\w+)\\:+(?<GenericType>L.*?(?=;\\w+|;$))");
    private static Pattern extendsGenericType = Pattern.compile("(?<GenericLetter>\\w+)\\:+(?<GenericType>L.*?(?=(?=;\\w+|;$)|(?=<.*>)))");

    private String signature;
    private String className;
    private ReferenceType parent;
    private List<GenericDefinition> genericDefinitions = null;
    private List<ReferenceType> interfaces = null;

    public class GenericDefinition {
        private String label;
        private Type type;

        GenericDefinition(String label, Type type) {
            this.label = label;
            this.type = type;
        }

        public String getLabel() {
            return label;
        }

        public Type getType() {
            return type;
        }
    }

    public TypeDefinition(String signature, String className) {
        this.signature = signature;
        this.className = className;
        this.checkSignature();
    }

    public ReferenceType getParent() {
        return parent;
    }

    public List<ReferenceType> getInterfaces() {
        return interfaces;
    }

    public List<GenericDefinition> getGenericDefinitions() {
        return genericDefinitions;
    }

    public String getClassName() { return this.className; }

    private void checkSignature() {
        Matcher matcher = typeSignature.matcher(this.signature);
        if(matcher.matches()){
            String typeGenericsSignature = matcher.group(2);
            String interfacesSignature = matcher.group(3);
            if (typeGenericsSignature != null) {
                this.setTypeGenerics(typeGenericsSignature);
            }
            if (interfacesSignature != null) {
                this.interfaces = GenericUtilities.getTypeParameters(interfacesSignature);
                if(this.interfaces.size() > 0){
                    // we assume that the first type parameter is the base class and the others are interfaces
                    parent = this.interfaces.get(0);
                    if ((parent instanceof ObjectType) &&
                            (((ObjectType)parent).getClassName().equals("java.lang.Enum")
                            || ((ObjectType)parent).getClassName().equals(DtsApi.JavaLangObject))){
                        if(((ObjectType)parent).getClassName().equals("java.lang.Enum")) {
                            parent = null; // we don't need extends for enum
                        }
                        // if we have only one another interface set it as a parent
                        // otherwise we don't know which one to set as a parent
                        if(this.interfaces.size() == 2) {
                            parent = this.interfaces.get(1);
                            this.interfaces.remove(1);
                        }
                    }
                    this.interfaces.remove(0);
                }
            }
        }
    }

    private void setTypeGenerics(String typeGenericsSignature) {
        Matcher matcher = extendsGenericType.matcher(typeGenericsSignature);
        this.genericDefinitions = new ArrayList<>();
        while (matcher.find()) {
            String label = matcher.group(1);
            String bound = matcher.group(2);

            // A type parameter may carry an intersection bound -- React Native's
            // ReactScrollViewHelper has <T extends ViewGroup & HasSmoothScroll> --
            // which arrives here as the bounds joined by ";:". A TypeScript type
            // parameter takes a single constraint, and the first bound is the one
            // the erased signature uses, so the rest are dropped.
            int intersection = bound.indexOf(";:");
            if (intersection >= 0) {
                bound = bound.substring(0, intersection);
            }

            Type type;
            try {
                type = GenericUtilities.getType(bound + ";");
            } catch (IllegalArgumentException unparsed) {
                // Some other shape the parser does not model. One unusable
                // constraint is not worth failing the whole run over.
                continue;
            }
            this.genericDefinitions.add(new GenericDefinition(label, type));
        }
    }
}
