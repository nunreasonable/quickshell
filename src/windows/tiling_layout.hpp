#pragma once

#include <memory>

#include <qlist.h>
#include <qpoint.h>
#include <qrect.h>
#include <qset.h>
#include <qtypes.h>

namespace qs::windows::tiling {

enum class Edge : quint8 { Left, Right, Top, Bottom };

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
	[[nodiscard]] QList<Id> ids() const;

	void insert(Id id, Id target, const QPoint* cursor = nullptr);
	void remove(Id id);
	void swap(Id a, Id b);
	bool replace(Id id, Id with);

	bool toggleSplit(Id id);
	bool swapSplit(Id id);
	bool splitRatio(Id id, double value, bool exact);

	bool moveEdge(Id id, Edge edge, int position);
	bool resize(Id id, int dx, int dy);

	void setPreserveSplit(bool preserve) { this->preserveSplit = preserve; }

	void setHidden(Id id, bool hidden);
	[[nodiscard]] bool isHidden(Id id) const { return this->hidden.contains(id); }

	void compute(const QRect& area);
	[[nodiscard]] QRect area() const { return this->mArea; }
	[[nodiscard]] QRect box(Id id) const;

private:
	struct Node {
		Node* parent = nullptr;
		std::unique_ptr<Node> first;
		std::unique_ptr<Node> second;
		Id id = 0;
		bool splitTop = false;
		bool splitPinned = false;
		double ratio = 0.5;
		QRect box;

		[[nodiscard]] bool isLeaf() const { return this->first == nullptr; }
	};

	[[nodiscard]] Node* find(Id id) const;
	[[nodiscard]] Node* lastLeaf() const;
	[[nodiscard]] Node* dividerAncestor(Node* leaf, Edge edge) const;
	static void setDivider(Node* node, int position);
	void computeNode(Node* node, const QRect& box) const;
	[[nodiscard]] bool hasVisible(const Node* node) const;
	static void collect(const Node* node, QList<Id>& out);

	std::unique_ptr<Node> root;
	QRect mArea;
	bool preserveSplit = true;
	QSet<Id> hidden;
};

} // namespace qs::windows::tiling
