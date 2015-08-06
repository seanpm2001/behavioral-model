/* Copyright 2013-present Barefoot Networks, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Antonin Bas (antonin@barefootnetworks.com)
 *
 */

#ifndef _BM_PARSER_H_
#define _BM_PARSER_H_

#include <utility>
#include <iostream>
#include <string>

#include <cassert>

#include "packet.h"
#include "phv.h"
#include "named_p4object.h"
#include "event_logger.h"

struct field_t {
  header_id_t header;
  int offset;
  
  static field_t make(header_id_t header, int offset) {
    field_t field = {header, offset};
    return field;
  }
};

namespace {

void extract(const char *data, int bit_offset, int bitwidth, char *dst) {
  int nbytes = (bitwidth + 7) / 8;

  if(bit_offset == 0 && bitwidth % 8 == 0) {
    std::copy(data, data + nbytes, dst);
  }

  int dst_offset = (nbytes << 3) - bitwidth;
  int i;
  
  // necessary to ensure correct behavior when shifting right (no sign extension)
  unsigned char *udata = (unsigned char *) data;

  int offset = bit_offset - dst_offset;
  if (offset == 0) {
    std::copy(udata, udata + nbytes, dst);
    dst[0] &= (0xFF >> dst_offset);
  }
  else if (offset > 0) { /* shift left */
    for (i = 0; i < nbytes - 1; i++) {
      dst[i] = (udata[i] << offset) | (udata[i + 1] >> (8 - offset));
    }
    dst[0] &= (0xFF >> dst_offset);
    dst[i] = udata[i] << offset;
    if((bit_offset + bitwidth) > (nbytes << 3)) {
      dst[i] |= (udata[i + 1] >> (8 - offset));
    }
  }
  else { /* shift right */
    offset = -offset;
    dst[0] = udata[0] >> offset;
      for (i = 1; i < nbytes; i++) {
	dst[i] = (udata[i - 1] << (8 - offset)) | (udata[i] >> offset);
      }
  }
}

}

struct ParserLookAhead {
  int byte_offset;
  int bit_offset;  
  int bitwidth;
  size_t nbytes;

  ParserLookAhead(int offset, int bitwidth)
    : byte_offset(offset / 8), bit_offset(offset % 8),
      bitwidth(bitwidth),
      nbytes((bitwidth + 7) / 8) { }

  void peek(const char *data, ByteContainer &res) const {
    size_t old_size = res.size();
    res.resize(old_size + nbytes);
    char *dst = &res[old_size];
    extract(data + byte_offset, bit_offset, bitwidth, dst);
  }
};

struct ParserOp {
  virtual ~ParserOp() {};
  virtual void operator()(Packet *pkt, const char *data,
			  size_t *bytes_parsed) const = 0;
};

struct ParserOpExtract : ParserOp {
  header_id_t header;

  ParserOpExtract(header_id_t header)
    : header(header) {}

  void operator()(Packet *pkt, const char *data,
		  size_t *bytes_parsed) const override
  {
    PHV *phv = pkt->get_phv();
    ELOGGER->parser_extract(*pkt, header);
    Header &hdr = phv->get_header(header);
    hdr.extract(data);
    *bytes_parsed += hdr.get_nbytes_packet();
  }
};

// push back a header on a tag stack
// TODO: probably room for improvement here
struct ParserOpExtractStack : ParserOp {
  header_stack_id_t header_stack;

  ParserOpExtractStack(header_stack_id_t header_stack)
    : header_stack(header_stack) {}

  void operator()(Packet *pkt, const char *data,
		  size_t *bytes_parsed) const override
  {
    PHV *phv = pkt->get_phv();
    HeaderStack &stack = phv->get_header_stack(header_stack);
    Header &next_hdr = stack.get_next(); // TODO: will assert if full
    ELOGGER->parser_extract(*pkt, next_hdr.get_id());
    next_hdr.extract(data);
    *bytes_parsed += next_hdr.get_nbytes_packet();
    stack.push_back(); // should I have a HeaderStack::extract() method instead?
  }
};

template <typename T>
struct ParserOpSet : ParserOp {
  field_t dst;
  T src;
  
  ParserOpSet(header_id_t header, int offset, const T &src)
    : dst({header, offset}), src(src) { 
    // dst = {header, offset};
  }

  void operator()(Packet *pkt, const char *data,
		  size_t *bytes_parsed) const override;
};

struct ParseSwitchKeyBuilder
{
  struct Entry {
    // I could use a union, but I only have 2 choices, and they are both quite
    // small. Plus I make a point of not using unions for non POD data. So I
    // could either change ParserLookAhead to POD data or go through the trouble
    // of implementing a union with setters, a copy assignment operator, a
    // dtor... I'll see if it is worth it later
    // The larger picture: is there really an improvement in performance. versus
    // using derived classes and virtual methods ? I guess there is a small one,
    // not because of the use of the vtable, but because I don't have to store
    // pointers, but I can store the objects in the vector directly
    // After some testing, it appears that using this is at least a couple times
    // faster
    enum {FIELD, LOOKAHEAD, STACK_FIELD} tag{};
    field_t field{0, 0};
    ParserLookAhead lookahead{0, 0};

    static Entry make_field(header_id_t header, int offset) {
      Entry e;
      e.tag = FIELD;
      e.field.header = header;
      e.field.offset = offset;
      return e;
    }

    static Entry make_stack_field(header_stack_id_t header_stack, int offset) {
      Entry e;
      e.tag = STACK_FIELD;
      e.field.header = header_stack;
      e.field.offset = offset;
      return e;
    }

    static Entry make_lookahead(int offset, int bitwidth) {
      Entry e;
      e.tag = LOOKAHEAD;
      e.lookahead = ParserLookAhead(offset, bitwidth);
      return e;
    }
  };

  std::vector<Entry> entries{};

  void push_back_field(header_id_t header, int field_offset) {
    entries.push_back(Entry::make_field(header, field_offset));
  }

  void push_back_stack_field(header_stack_id_t header_stack, int field_offset) {
    entries.push_back(Entry::make_stack_field(header_stack, field_offset));
  }

  void push_back_lookahead(int offset, int bitwidth) {
    entries.push_back(Entry::make_lookahead(offset, bitwidth));
  }
  
  void operator()(const PHV &phv, const char *data, ByteContainer &key) const
  {
    for(const Entry &e : entries) {
      switch(e.tag) {
      case Entry::FIELD:
	key.append(phv.get_field(e.field.header, e.field.offset).get_bytes());
	break;
      case Entry::STACK_FIELD:
	key.append(phv.get_header_stack(e.field.header).get_last().get_field(e.field.offset).get_bytes());
	break;
      case Entry::LOOKAHEAD:
	e.lookahead.peek(data, key);
	break;
      }
    }
  }
};

class ParseState;

class ParseSwitchCase {
public:
  ParseSwitchCase(const ByteContainer &key, const ParseState *next_state)
    : key(key), with_mask(false), next_state(next_state) {
  }

  ParseSwitchCase(int nbytes_key, const char *key, const ParseState *next_state)
    : key(ByteContainer(key, nbytes_key)),
      with_mask(false), next_state(next_state) {
  }
  
  ParseSwitchCase(const ByteContainer &key,
		  const ByteContainer &mask,
		  const ParseState *next_state)
    : key(key), mask(mask), with_mask(true), next_state(next_state) {
	assert(key.size() == mask.size());
  }

  bool match(const ByteContainer &input, const ParseState **state) const;

  ParseSwitchCase(const ParseSwitchCase &other) = delete;
  ParseSwitchCase &operator=(const ParseSwitchCase &other) = delete;

  ParseSwitchCase(ParseSwitchCase &&other) /*noexcept*/ = default;
  ParseSwitchCase &operator=(ParseSwitchCase &&other) /*noexcept*/ = default;

private:
  ByteContainer key;
  ByteContainer mask{};
  bool with_mask;
  const ParseState *next_state; /* NULL if end */
};

class ParseState : public NamedP4Object {
public:
  ParseState(const std::string &name, p4object_id_t id)
    : NamedP4Object(name, id),
      has_switch(false) {}

  void add_extract(header_id_t header) {
    parser_ops.emplace_back(new ParserOpExtract(header));
  }

  void add_extract_to_stack(header_stack_id_t header_stack) {
    parser_ops.emplace_back(new ParserOpExtractStack(header_stack));
  }

  void add_set_from_field(header_id_t dst_header, int dst_offset,
			  header_id_t src_header, int src_offset) {
    parser_ops.emplace_back(
      new ParserOpSet<field_t>(dst_header, dst_offset,
			       field_t::make(src_header, src_offset))
    );
  }

  void add_set_from_data(header_id_t dst_header, int dst_offset,
			 const Data &src) {
    parser_ops.emplace_back(
      new ParserOpSet<Data>(dst_header, dst_offset, src)
    );
  }

  void add_set_from_lookahead(header_id_t dst_header, int dst_offset,
			      int src_offset, int src_bitwidth) {
    parser_ops.emplace_back(
      new ParserOpSet<ParserLookAhead>(dst_header, dst_offset,
				       ParserLookAhead(src_offset, src_bitwidth))
    );
  }

  void set_key_builder(const ParseSwitchKeyBuilder &builder) {
    has_switch = true;
    key_builder = builder;
  }

  void add_switch_case(const ByteContainer &key, const ParseState *next_state) {
    parser_switch.push_back( ParseSwitchCase(key, next_state) );
  }

  void add_switch_case(int nbytes_key, const char *key,
		       const ParseState *next_state) {
    parser_switch.push_back( ParseSwitchCase(nbytes_key, key, next_state) );
  }

  void set_default_switch_case(const ParseState *default_next) {
    default_next_state = default_next;
  }

  // Copy constructor
  ParseState (const ParseState& other) = delete;

  // Copy assignment operator
  ParseState &operator =(const ParseState& other) = delete;
 
  // Move constructor
  ParseState(ParseState&& other)= default;
 
  // Move assignment operator
  ParseState &operator =(ParseState &&other) = default;

  const ParseState *operator()(Packet *pkt, const char *data,
			       size_t *bytes_parsed) const;

private:
  std::vector<std::unique_ptr<ParserOp> > parser_ops{};
  bool has_switch;
  ParseSwitchKeyBuilder key_builder{};
  std::vector<ParseSwitchCase> parser_switch{};
  const ParseState *default_next_state{nullptr};
};

class Parser : public NamedP4Object {
public:
 Parser(const std::string &name, p4object_id_t id)
   : NamedP4Object(name, id), init_state(NULL) {}

  void set_init_state(const ParseState *state) {
    init_state = state;
  }

  void parse(Packet *pkt) const;

  Parser(const Parser &other) = delete;
  Parser &operator=(const Parser &other) = delete;

  Parser(Parser &&other) /*noexcept*/ = default;
  Parser &operator=(Parser &&other) /*noexcept*/ = default;

private:
  const ParseState *init_state;
};

#endif
