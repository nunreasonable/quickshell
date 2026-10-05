#include "tiling_layout.hpp"
#include <cmath>
#include <memory>
#include <utility>

#include <qlist.h>
#include <qpoint.h>
#include <qrect.h>
#include <qset.h>
#include <qtypes.h>

namespace qs::windows::tiling {

namespace {

// Hyprland clamps its split ratio to 0.1..1.9 of an even 1.0, i.e. 5%..95% of the box.
constexpr double MIN_RATIO = 0.05;
constexpr double MAX_RATIO = 0.95;

double clampRatio(double ratio) { return qBound(MIN_RATIO, ratio, MAX_RATIO); }

bool isVerticalEdge(Edge edge) { return edge == Edge::Left || edge == Edge::Right; }

} // namespace

qsizetype DwindleLayout::count() const { return this->ids().length(); }

QList<DwindleLayout::Id> DwindleLayout::ids() const {
	QList<Id> out;
	if (this->root != nullptr) DwindleLayout::collect(this->root.get(), out);
	return out;
}

void DwindleLayout::collect(const Node* node, QList<Id>& out) {
	if (node->isLeaf()) {
		out.append(node->id);
		return;
	}

	DwindleLayout::collect(node->first.get(), out);
	DwindleLayout::collect(node->second.get(), out);
}

DwindleLayout::Node* DwindleLayout::find(Id id) const {
	if (this->root == nullptr) return nullptr;

	// Trees stay small (a handful of windows per monitor), so a walk beats keeping an index
	// in sync through every restructuring.
	QList<Node*> stack {this->root.get()};
	while (!stack.isEmpty()) {
		auto* node = stack.takeLast();
		if (node->isLeaf()) {
			if (node->id == id) return node;
			continue;
		}

		stack.append(node->second.get());
		stack.append(node->first.get());
	}

	return nullptr;
}

DwindleLayout::Node* DwindleLayout::lastLeaf() const {
	auto* node = this->root.get();
	while (node != nullptr && !node->isLeaf()) node = node->second.get();
	return node;
}

void DwindleLayout::insert(Id id, Id target, const QPoint* cursor) {
	if (this->contains(id)) return;

	auto leaf = std::make_unique<Node>();
	leaf->id = id;

	if (this->root == nullptr) {
		this->root = std::move(leaf);
		return;
	}

	auto* split = this->find(target);
	if (split == nullptr) split = this->lastLeaf();

	// The target leaf becomes the split; its window moves into a new leaf below it.
	auto old = std::make_unique<Node>();
	old->id = split->id;
	old->box = split->box;
	old->parent = split;
	leaf->parent = split;

	const auto box = split->box;
	split->id = 0;
	split->splitTop = box.height() > box.width();
	split->splitPinned = false;
	split->ratio = 0.5;

	auto newFirst = false;
	if (cursor != nullptr && box.contains(*cursor)) {
		newFirst = split->splitTop ? cursor->y() < box.center().y() : cursor->x() < box.center().x();
	}

	if (newFirst) {
		split->first = std::move(leaf);
		split->second = std::move(old);
	} else {
		split->first = std::move(old);
		split->second = std::move(leaf);
	}
}

void DwindleLayout::remove(Id id) {
	auto* node = this->find(id);
	if (node == nullptr) return;

	this->hidden.remove(id);

	if (node == this->root.get()) {
		this->root.reset();
		return;
	}

	auto* parent = node->parent;
	auto sibling = parent->first.get() == node ? std::move(parent->second) : std::move(parent->first);
	sibling->parent = parent->parent;

	// Assigning over the parent's slot destroys the parent and, with it, the removed leaf.
	if (parent->parent == nullptr) {
		this->root = std::move(sibling);
	} else {
		auto* grand = parent->parent;
		auto& slot = grand->first.get() == parent ? grand->first : grand->second;
		slot = std::move(sibling);
	}
}

void DwindleLayout::swap(Id a, Id b) {
	auto* nodeA = this->find(a);
	auto* nodeB = this->find(b);
	if (nodeA == nullptr || nodeB == nullptr || nodeA == nodeB) return;
	std::swap(nodeA->id, nodeB->id);
}

bool DwindleLayout::replace(Id id, Id with) {
	auto* node = this->find(id);
	if (node == nullptr) return false;
	node->id = with;
	this->hidden.remove(id);
	return true;
}

void DwindleLayout::setHidden(Id id, bool hidden) {
	if (hidden && this->contains(id)) this->hidden.insert(id);
	else this->hidden.remove(id);
}

bool DwindleLayout::hasVisible(const Node* node) const {
	if (node->isLeaf()) return !this->hidden.contains(node->id);
	return this->hasVisible(node->first.get()) || this->hasVisible(node->second.get());
}

bool DwindleLayout::toggleSplit(Id id) {
	auto* node = this->find(id);
	if (node == nullptr || node->parent == nullptr) return false;
	node->parent->splitTop = !node->parent->splitTop;
	node->parent->splitPinned = true;
	return true;
}

bool DwindleLayout::swapSplit(Id id) {
	auto* node = this->find(id);
	if (node == nullptr || node->parent == nullptr) return false;
	std::swap(node->parent->first, node->parent->second);
	return true;
}

bool DwindleLayout::splitRatio(Id id, double value, bool exact) {
	auto* node = this->find(id);
	if (node == nullptr || node->parent == nullptr) return false;

	auto* parent = node->parent;
	auto hyprland = exact ? value : parent->ratio * 2.0 + value;
	parent->ratio = clampRatio(hyprland / 2.0);
	return true;
}

DwindleLayout::Node* DwindleLayout::dividerAncestor(Node* leaf, Edge edge) const {
	auto vertical = isVerticalEdge(edge);
	auto trailing = edge == Edge::Right || edge == Edge::Bottom;

	for (Node *child = leaf, *node = leaf->parent; node != nullptr;
	     child = node, node = node->parent)
	{
		// A side by side split has a vertical divider (left/right edges of its children).
		if (node->splitTop == vertical) continue;

		// The divider is on the trailing edge of the first child and the leading edge of the
		// second.
		auto childFirst = node->first.get() == child;
		if (childFirst != trailing) continue;

		// With everything on the other side hidden this leaf's side takes the whole box.
		if (this->hasVisible(childFirst ? node->second.get() : node->first.get())) return node;
	}

	return nullptr;
}

void DwindleLayout::setDivider(Node* node, int position) {
	const auto& box = node->box;
	auto length = node->splitTop ? box.height() : box.width();
	if (length <= 0) return;

	auto offset = position - (node->splitTop ? box.top() : box.left());
	node->ratio = clampRatio(static_cast<double>(offset) / length);
}

bool DwindleLayout::moveEdge(Id id, Edge edge, int position) {
	auto* leaf = this->find(id);
	if (leaf == nullptr) return false;

	auto* node = this->dividerAncestor(leaf, edge);
	if (node == nullptr) return false;

	DwindleLayout::setDivider(node, position);
	return true;
}

bool DwindleLayout::resize(Id id, int dx, int dy) {
	auto* leaf = this->find(id);
	if (leaf == nullptr) return false;

	auto changed = false;

	auto grow = [&](int delta, Edge trailing, Edge leading) {
		if (delta == 0) return;

		auto divider = [](const Node* node) {
			const auto& box = node->box;
			return node->splitTop ? box.top() + static_cast<int>(std::lround(box.height() * node->ratio))
			                      : box.left() + static_cast<int>(std::lround(box.width() * node->ratio));
		};

		if (auto* node = this->dividerAncestor(leaf, trailing)) {
			DwindleLayout::setDivider(node, divider(node) + delta);
			changed = true;
		} else if (auto* node = this->dividerAncestor(leaf, leading)) {
			DwindleLayout::setDivider(node, divider(node) - delta);
			changed = true;
		}
	};

	grow(dx, Edge::Right, Edge::Left);
	grow(dy, Edge::Bottom, Edge::Top);
	return changed;
}

void DwindleLayout::compute(const QRect& area) {
	this->mArea = area;
	if (this->root != nullptr) this->computeNode(this->root.get(), area);
}

void DwindleLayout::computeNode(Node* node, const QRect& box) const {
	if (node->isLeaf()) {
		node->box = this->hidden.contains(node->id) ? QRect() : box;
		return;
	}

	node->box = box;

	// A half with nothing to show gives the other half the whole box; its own split direction
	// and ratio stay for when it shows something again.
	auto firstVisible = this->hasVisible(node->first.get());
	auto secondVisible = this->hasVisible(node->second.get());
	if (!firstVisible || !secondVisible) {
		this->computeNode(node->first.get(), firstVisible ? box : QRect());
		this->computeNode(node->second.get(), secondVisible ? box : QRect());
		return;
	}

	if (!this->preserveSplit && !node->splitPinned) node->splitTop = box.height() > box.width();

	// Integer split along one axis; the second child gets the remainder so the two boxes always
	// share their edge exactly.
	if (node->splitTop) {
		auto firstHeight = static_cast<int>(std::lround(box.height() * node->ratio));
		this->computeNode(node->first.get(), QRect(box.left(), box.top(), box.width(), firstHeight));
		this->computeNode(
		    node->second.get(),
		    QRect(box.left(), box.top() + firstHeight, box.width(), box.height() - firstHeight)
		);
	} else {
		auto firstWidth = static_cast<int>(std::lround(box.width() * node->ratio));
		this->computeNode(node->first.get(), QRect(box.left(), box.top(), firstWidth, box.height()));
		this->computeNode(
		    node->second.get(),
		    QRect(box.left() + firstWidth, box.top(), box.width() - firstWidth, box.height())
		);
	}
}

QRect DwindleLayout::box(Id id) const {
	auto* node = this->find(id);
	return node == nullptr ? QRect() : node->box;
}

} // namespace qs::windows::tiling
