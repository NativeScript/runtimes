package org.nativescript.reactnative;

import android.content.Context;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewParent;

import androidx.annotation.Nullable;

/** Shows the view a NativeScriptUIView's handle names, filling the host. */
final class NativeScriptUIHostView extends ViewGroup {
    @Nullable
    private View hosted;
    private boolean layoutPending;

    private final Runnable measureAndLayout = () -> {
        layoutPending = false;
        measure(MeasureSpec.makeMeasureSpec(getWidth(), MeasureSpec.EXACTLY),
                MeasureSpec.makeMeasureSpec(getHeight(), MeasureSpec.EXACTLY));
        layout(getLeft(), getTop(), getRight(), getBottom());
    };

    NativeScriptUIHostView(Context context) {
        super(context);
    }

    void setNativeViewHandle(@Nullable String handle) {
        View next = NativeScriptViewRegistry.resolve(handle);
        if (next == hosted && (next == null || next.getParent() == this)) {
            return;
        }
        if (hosted != null && hosted.getParent() == this) {
            removeView(hosted);
        }
        hosted = next;
        if (next != null) {
            // A view is in one place at a time; a handle shown by a new host
            // moves there, as a UIView does on iOS.
            ViewParent parent = next.getParent();
            if (parent instanceof ViewGroup) {
                ((ViewGroup) parent).removeView(next);
            }
            addView(next, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
        }
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        setMeasuredDimension(MeasureSpec.getSize(widthMeasureSpec),
                MeasureSpec.getSize(heightMeasureSpec));
        if (hosted != null && hosted.getParent() == this) {
            hosted.measure(MeasureSpec.makeMeasureSpec(getMeasuredWidth(), MeasureSpec.EXACTLY),
                    MeasureSpec.makeMeasureSpec(getMeasuredHeight(), MeasureSpec.EXACTLY));
        }
    }

    @Override
    protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
        if (hosted != null && hosted.getParent() == this) {
            hosted.layout(0, 0, right - left, bottom - top);
        }
    }

    @Override
    public void requestLayout() {
        super.requestLayout();
        // React Native positions its own views and ends a layout request at the
        // first of them, so one from the hosted view (new text, a new child)
        // would otherwise never be served. Null while ViewGroup's constructor runs.
        if (measureAndLayout != null && !layoutPending) {
            layoutPending = true;
            post(measureAndLayout);
        }
    }
}
