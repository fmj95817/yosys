/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Clifford Wolf <clifford@clifford.at>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/yosys.h"

YOSYS_NAMESPACE_BEGIN

struct JsonNode
{
	char type; // S=String, N=Number, A=Array, D=Dict
	string data_string;
	int data_number;
	vector<JsonNode*> data_array;
	dict<string, JsonNode*> data_dict;

	JsonNode(std::istream &f)
	{
		type = 0;
		data_number = 0;

		while (1)
		{
			int ch = f.get();

			if (ch == EOF)
				log_error("Unexpected EOF in JSON file.\n");

			if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
				continue;

			if (ch == '"')
			{
				type = 'S';

				while (1)
				{
					ch = f.get();

					if (ch == EOF)
						log_error("Unexpected EOF in JSON string.\n");

					if (ch == '"')
						break;

					if (ch == '\\') {
						int ch = f.get();

						if (ch == EOF)
							log_error("Unexpected EOF in JSON string.\n");
					}

					data_string += ch;
				}

				break;
			}

			if ('0' <= ch && ch <= '9')
			{
				type = 'N';
				data_number = ch - '0';

				while (1)
				{
					ch = f.get();

					if (ch == EOF)
						break;

					if (ch < '0' || '9' < ch) {
						f.unget();
						break;
					}

					data_number = data_number*10 + (ch - '0');
				}

				break;
			}

			if (ch == '[')
			{
				type = 'A';

				while (1)
				{
					ch = f.get();

					if (ch == EOF)
						log_error("Unexpected EOF in JSON file.\n");

					if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == ',')
						continue;

					if (ch == ']')
						break;

					f.unget();
					data_array.push_back(new JsonNode(f));
				}

				break;
			}

			if (ch == '{')
			{
				type = 'D';

				while (1)
				{
					ch = f.get();

					if (ch == EOF)
						log_error("Unexpected EOF in JSON file.\n");

					if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == ',')
						continue;

					if (ch == '}')
						break;

					f.unget();
					JsonNode key(f);

					while (1)
					{
						ch = f.get();

						if (ch == EOF)
							log_error("Unexpected EOF in JSON file.\n");

						if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == ':')
							continue;

						f.unget();
						break;
					}

					JsonNode *value = new JsonNode(f);

					if (key.type != 'S')
						log_error("Unexpected non-string key in JSON dict.\n");

					data_dict[key.data_string] = value;
				}

				break;
			}

			log_error("Unexpected character in JSON file: '%c'\n", ch);
		}
	}

	~JsonNode()
	{
		for (auto it : data_array)
			delete it;
		for (auto &it : data_dict)
			delete it.second;
	}
};

void json_import(Design *design, string &modname, JsonNode *node)
{
	log("Importing module %s from JSON tree.\n", modname.c_str());

	Module *module = new RTLIL::Module;
	module->name = RTLIL::escape_id(modname.c_str());

	if (design->module(module->name))
		log_error("Re-definition of module %s.\n", log_id(module->name));

	design->add(module);

	// FIXME: Handle module attributes

	dict<int, SigBit> signal_bits;

	if (node->data_dict.count("ports"))
	{
		JsonNode *ports_node = node->data_dict.at("ports");

		if (ports_node->type != 'D')
			log_error("JSON ports node is not a dictionary.\n");

		for (auto &port : ports_node->data_dict)
		{
			IdString port_name = RTLIL::escape_id(port.first.c_str());
			JsonNode *port_node = port.second;

			if (port_node->type != 'D')
				log_error("JSON port node '%s' is not a dictionary.\n", log_id(port_name));

			if (port_node->data_dict.count("direction") == 0)
				log_error("JSON port node '%s' has no direction attribute.\n", log_id(port_name));

			if (port_node->data_dict.count("bits") == 0)
				log_error("JSON port node '%s' has no bits attribute.\n", log_id(port_name));

			JsonNode *port_direction_node = port_node->data_dict.at("direction");
			JsonNode *port_bits_node = port_node->data_dict.at("bits");

			if (port_direction_node->type != 'S')
				log_error("JSON port node '%s' has non-string direction attribute.\n", log_id(port_name));

			if (port_bits_node->type != 'A')
				log_error("JSON port node '%s' has non-array bits attribute.\n", log_id(port_name));

			Wire *port_wire = module->wire(port_name);

			if (port_wire == nullptr)
				port_wire = module->addWire(port_name, GetSize(port_bits_node->data_array));

			if (port_direction_node->data_string == "input") {
				port_wire->port_input = true;
			} else
			if (port_direction_node->data_string == "output") {
				port_wire->port_output = true;
			} else
			if (port_direction_node->data_string == "inout") {
				port_wire->port_output = true;
			} else
				log_error("JSON port node '%s' has invalid '%s' direction attribute.\n", log_id(port_name), port_direction_node->data_string.c_str());

			for (int i = 0; i < GetSize(port_bits_node->data_array); i++)
			{
				JsonNode *bitval_node = port_bits_node->data_array.at(i);
				SigBit sigbit(port_wire, i);

				if (bitval_node->type == 'S') {
					if (bitval_node->data_string == "0")
						module->connect(sigbit, State::S0);
					else if (bitval_node->data_string == "1")
						module->connect(sigbit, State::S1);
					else if (bitval_node->data_string == "x")
						module->connect(sigbit, State::Sx);
					else if (bitval_node->data_string == "z")
						module->connect(sigbit, State::Sz);
					else
						log_error("JSON port node '%s' has invalid '%s' bit string value on bit %d.\n",
								log_id(port_name), bitval_node->data_string.c_str(), i);
				} else
				if (bitval_node->type == 'N') {
					int bitidx = bitval_node->data_number;
					if (signal_bits.count(bitidx)) {
						if (port_wire->port_output) {
							module->connect(sigbit, signal_bits.at(bitidx));
						} else {
							module->connect(signal_bits.at(bitidx), sigbit);
							signal_bits[bitidx] = sigbit;
						}
					} else {
						signal_bits[bitidx] = sigbit;
					}
				} else
					log_error("JSON port node '%s' has invalid bit value on bit %d.\n", log_id(port_name), i);
			}
		}

		module->fixup_ports();
	}

	if (node->data_dict.count("netnames"))
	{
		JsonNode *netnames_node = node->data_dict.at("netnames");

		if (netnames_node->type != 'D')
			log_error("JSON netnames node is not a dictionary.\n");

		for (auto &net : netnames_node->data_dict)
		{
			IdString net_name = RTLIL::escape_id(net.first.c_str());
			JsonNode *net_node = net.second;

			if (net_node->type != 'D')
				log_error("JSON netname node '%s' is not a dictionary.\n", log_id(net_name));

			if (net_node->data_dict.count("bits") == 0)
				log_error("JSON netname node '%s' has no bits attribute.\n", log_id(net_name));

			JsonNode *bits_node = net_node->data_dict.at("bits");

			if (bits_node->type != 'A')
				log_error("JSON netname node '%s' has non-array bits attribute.\n", log_id(net_name));

			Wire *wire = module->wire(net_name);

			if (wire == nullptr)
				wire = module->addWire(net_name, GetSize(bits_node->data_array));

			for (int i = 0; i < GetSize(bits_node->data_array); i++)
			{
				JsonNode *bitval_node = bits_node->data_array.at(i);
				SigBit sigbit(wire, i);

				if (bitval_node->type == 'S') {
					if (bitval_node->data_string == "0")
						module->connect(sigbit, State::S0);
					else if (bitval_node->data_string == "1")
						module->connect(sigbit, State::S1);
					else if (bitval_node->data_string == "x")
						module->connect(sigbit, State::Sx);
					else if (bitval_node->data_string == "z")
						module->connect(sigbit, State::Sz);
					else
						log_error("JSON netname node '%s' has invalid '%s' bit string value on bit %d.\n",
								log_id(net_name), bitval_node->data_string.c_str(), i);
				} else
				if (bitval_node->type == 'N') {
					int bitidx = bitval_node->data_number;
					if (signal_bits.count(bitidx)) {
						if (sigbit != signal_bits.at(bitidx))
							module->connect(sigbit, signal_bits.at(bitidx));
					} else {
						signal_bits[bitidx] = sigbit;
					}
				} else
					log_error("JSON netname node '%s' has invalid bit value on bit %d.\n", log_id(net_name), i);
			}

			// FIXME: Handle wire attributes
		}
	}

	if (node->data_dict.count("cells"))
	{
		JsonNode *cells_node = node->data_dict.at("cells");

		if (cells_node->type != 'D')
			log_error("JSON cells node is not a dictionary.\n");

		for (auto &cell_node_it : cells_node->data_dict)
		{
			IdString cell_name = RTLIL::escape_id(cell_node_it.first.c_str());
			JsonNode *cell_node = cell_node_it.second;

			if (cell_node->type != 'D')
				log_error("JSON cells node '%s' is not a dictionary.\n", log_id(cell_name));

			if (cell_node->data_dict.count("type") == 0)
				log_error("JSON cells node '%s' has no type attribute.\n", log_id(cell_name));

			JsonNode *type_node = cell_node->data_dict.at("type");

			if (type_node->type != 'S')
				log_error("JSON cells node '%s' has a non-string type.\n", log_id(cell_name));

			IdString cell_type = RTLIL::escape_id(type_node->data_string.c_str());

			Cell *cell = module->addCell(cell_name, cell_type);

			if (cell_node->data_dict.count("connections") == 0)
				log_error("JSON cells node '%s' has no connections attribute.\n", log_id(cell_name));

			JsonNode *connections_node = cell_node->data_dict.at("connections");

			if (connections_node->type != 'D')
				log_error("JSON cells node '%s' has non-dictionary connections attribute.\n", log_id(cell_name));

			for (auto &conn_it : connections_node->data_dict)
			{
				IdString conn_name = RTLIL::escape_id(conn_it.first.c_str());
				JsonNode *conn_node = conn_it.second;

				if (conn_node->type != 'A')
					log_error("JSON cells node '%s' connection '%s' is not an array.\n", log_id(cell_name), log_id(conn_name));

				SigSpec sig;

				for (int i = 0; i < GetSize(conn_node->data_array); i++)
				{
					JsonNode *bitval_node = conn_node->data_array.at(i);

					if (bitval_node->type == 'S') {
						if (bitval_node->data_string == "0")
							sig.append(State::S0);
						else if (bitval_node->data_string == "1")
							sig.append(State::S1);
						else if (bitval_node->data_string == "x")
							sig.append(State::Sx);
						else if (bitval_node->data_string == "z")
							sig.append(State::Sz);
						else
							log_error("JSON cells node '%s' connection '%s' has invalid '%s' bit string value on bit %d.\n",
									log_id(cell_name), log_id(conn_name), bitval_node->data_string.c_str(), i);
					} else
					if (bitval_node->type == 'N') {
						int bitidx = bitval_node->data_number;
						if (signal_bits.count(bitidx) == 0)
							signal_bits[bitidx] = module->addWire(NEW_ID);
						sig.append(signal_bits.at(bitidx));
					} else
						log_error("JSON cells node '%s' connection '%s' has invalid bit value on bit %d.\n",
								log_id(cell_name), log_id(conn_name), i);

				}

				cell->setPort(conn_name, sig);
			}

			// FIXME: Handle cell attributes
			// FIXME: Handle cell parameters
		}
	}
}

struct JsonFrontend : public Frontend {
	JsonFrontend() : Frontend("json", "read JSON file") { }
	virtual void help()
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    read_json [filename]\n");
		log("\n");
		log("Load modules from a JSON file into the current design See \"help write_json\"\n");
		log("for a description of the file format.\n");
		log("\n");
	}
	virtual void execute(std::istream *&f, std::string filename, std::vector<std::string> args, RTLIL::Design *design)
	{
		log_header(design, "Executing JSON frontend.\n");

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			// std::string arg = args[argidx];
			// if (arg == "-sop") {
			// 	sop_mode = true;
			// 	continue;
			// }
			break;
		}
		extra_args(f, filename, args, argidx);

		JsonNode root(*f);

		if (root.type != 'D')
			log_error("JSON root node is not a dictionary.\n");

		if (root.data_dict.count("modules") != 0)
		{
			JsonNode *modules = root.data_dict.at("modules");

			if (modules->type != 'D')
				log_error("JSON modules node is not a dictionary.\n");

			for (auto &it : modules->data_dict)
				json_import(design, it.first, it.second);
		}
	}
} JsonFrontend;

YOSYS_NAMESPACE_END
