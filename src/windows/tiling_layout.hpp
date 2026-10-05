#pragma once

#include <memory>

#include <qlist.h>
#include <qpoint.h>
#include <qrect.h>
#include <qtypes.h>

namespace qs::windows::tiling {

enum class Edge : quint8 { Left, Right, Top, Bottom };

// Hyprland's "dwindle" layout as a binary tree, without any Windows API in it: leaves hold
// opaque window ids, inner nodes split their box in two. Every new window splits the box of a
// target leaf, side by side when that box is wider than tall. With preserveSplit (Hyprland's
// dwindle:preserve_split, which ii's config sets) a split keeps that direction afterwards;
// without it the direction follows its box's aspect ratio on every compute(), unless the split
// was toggled. Boxes are in whatever integer space compute() is given (physical pixels for the
// manager); gaps are applied by the caller.
class DwindleLayout {
public:
	using Id = quintptr;

	DwindleLayout() = default;
	~DwindleLayout() = default;
	DwindleLayout(const DwindleLayout&) = delete;
	DwindleLayout& operator=(const DwindleLayout&) = delete;
	DwindleLayout(DwindleLayout&&) = default;
	DwindleLayout& operator=(DwindleLayout&&) = default;

	[[nodiscard]] bool isEmpty() const { return this->root == nullptr; }
	[[nodiscard]] bool contains(Id id) const { return this->find(id) != nullptr; }
	[[nodiscard]] qsizetype count() const;
	// Leaves in tree order (left/top first).
	[[nodiscard]] QList<Id> ids() const;

	// Splits `target`'s box (the last leaf when `target` isn't in the layout). The new window
	// takes the second half (right/bottom) unless `cursor` lies in the first half of the target.
	void insert(Id id, Id target, const QPoint* cursor = nullptr);
	// The sibling takes over the parent's box.
	void remove(Id id);
	// Both in this layout.
	void swap(Id a, Id b);
	// Puts `with` in the leaf of `id` (for swaps between layouts).
	bool replace(Id id, Id with);

	// Flips the split of the leaf's parent and keeps it that way.
	bool toggleSplit(Id id);
	// Swaps the two halves of the leaf's parent.
	bool swapSplit(Id id);
	// Hyprland's dwindle `splitratio`: changes the parent's ratio by `delta` (relative) or sets it,
	// on Hyprland's scale where 1 is an even split (0.1 to 1.9).
	bool splitRatio(Id id, double value, bool exact);

	// Moves the divider on that edge of the leaf's box to `position` (same space as the boxes).
	// False when that edge is an outer edge of the layout.
	bool moveEdge(Id id, Edge edge, int position);
	// Grows the leaf's box by dx/dy (negative shrinks), moving its right/bottom divider when it
	// has one, otherwise its left/top one.
	bool resize(Id id, int dx, int dy);

	void setPreserveSplit(bool preserve) { this->preserveSplit = preserve; }

	// Lays the tree out in `area`.
	void compute(const QRect& area);
	[[nodiscard]] QRect area() const { return this->mArea; }
	// Box of a leaf from the last compute(), null if not in the layout.
	[[nodiscard]] QRect box(Id id) const;

private:
	struct Node {
		Node* parent = nullptr;
		std::unique_ptr<Node> first;
		std::unique_ptr<Node> second;
		Id id = 0; // leaves only
		bool splitTop = false; // children stacked (first on top) instead of side by side
		bool splitPinned = false;
		double ratio = 0.5; // share of the first child
		QRect box;

		[[nodiscard]] bool isLeaf() const { return this->first == nullptr; }
	};

	[[nodiscard]] Node* find(Id id) const;
	[[nodiscard]] Node* lastLeaf() const;
	// Nearest ancestor whose divider is on that edge of the leaf.
	[[nodiscard]] static Node* dividerAncestor(Node* leaf, Edge edge);
	static void setDivider(Node* node, int position);
	void computeNode(Node* node, const QRect& box) const;
	static void collect(const Node* node, QList<Id>& out);

	std::unique_ptr<Node> root;
	QRect mArea;
	bool preserveSplit = true;
};

} // namespace qs::windows::tiling
